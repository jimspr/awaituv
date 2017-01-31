#pragma once
#include <uv.h> // libuv
#include <functional>
#include <memory>
#include <list>
#include <string.h>

#ifdef __unix__
#include <coroutine.h>
#else
#include <experimental\resumable>
#endif

namespace awaituv
{
  enum struct future_error
  {
    not_ready,			// get_value called when value not available
    already_acquired	// attempt to get another future
  };

  struct future_exception : std::exception
  {
    future_error _error;
    future_exception(future_error fe) : _error(fe)
    {
    }
  };

  struct awaitable_state_base
  {
    std::function<void(void)> _coro;
    bool _ready = false;
    bool _future_acquired = false;

    awaitable_state_base() = default;
    awaitable_state_base(awaitable_state_base&&) = delete;
    awaitable_state_base(const awaitable_state_base&) = delete;

    void set_coro(std::function<void(void)> cb)
    {
      _coro = cb;
    }

    void set_value()
    {
      // Set all members first as calling coroutine may reset stuff here.
      _ready = true;
      auto coro = _coro;
      _coro = nullptr;
      if (coro != nullptr)
        coro();
    }

    bool ready() const
    {
      return _ready;
    }

    void reset()
    {
      _coro = nullptr;
      _ready = false;
      _future_acquired = false;
    }

    // functions that make this directly awaitable
    bool await_ready()
    {
      return _ready;
    }

    void await_suspend(std::experimental::coroutine_handle<> resume_cb)
    {
      set_coro(resume_cb);
    }
  };

  template <typename T>
  struct awaitable_state : public awaitable_state_base
  {
    T _value;

    void set_value(const T& t)
    {
      _value = t;
      awaitable_state_base::set_value();
    }

    auto get_value()
    {
      if (!_ready)
        throw future_exception{ future_error::not_ready };
      return _value;
    }

    void reset()
    {
      awaitable_state_base::reset();
      _value = T{};
    }

    // make this directly awaitable
    auto await_resume()
    {
      return _value;
    }
  };

  // specialization of awaitable_state<void>
  template <>
  struct awaitable_state<void> : public awaitable_state_base
  {
    void get_value()
    {
      if (!_ready)
        throw future_exception{ future_error::not_ready };
    }

    // make this directly awaitable
    void await_resume()
    {
    }
  };

  // The awaitable_state class is good enough for most cases, however there are some cases
  // where a libuv callback returns more than one "value".  In that case, the function can
  // define its own state type that holds more information.
  template <typename T, typename state_t = awaitable_state<T>>
  struct promise_t;

  template <typename T, typename state_t = awaitable_state<T>>
  struct future_t
  {
    typedef promise_t<T, state_t> promise_type;
    std::shared_ptr<state_t> _state;

    future_t(const std::shared_ptr<state_t>& state) : _state(state)
    {
      _state->_future_acquired = true;
    }

    // movable, but not copyable
    future_t(const future_t&) = delete;
    future_t(future_t&& f) = default;

    auto await_resume()
    {
      return _state->get_value();
    }

    bool await_ready()
    {
      return _state->_ready;
    }

    void await_suspend(std::experimental::coroutine_handle<> resume_cb)
    {
      _state->set_coro(resume_cb);
    }

    bool ready()
    {
      return _state->_ready;
    }

    auto get_value()
    {
      return _state->get_value();
    }
  };

  template <typename T, typename state_t>
  struct promise_t
  {
    typedef future_t<T, state_t> future_type;
    std::shared_ptr<state_t> _state{ std::make_shared<state_t>() };

    // movable not copyable
    promise_t() = default;
    promise_t(const promise_t&) = delete;
    promise_t(promise_t&&) = default;

    void set_value(const T& value)
    {
      _state->set_value(value);
    }

    future_type get_future()
    {
      if (_state->_future_acquired)
        throw future_exception{ future_error::already_acquired };
      return future_type(_state);
    }

    // Most functions don't need this but timers and reads from streams
    // cause multiple callbacks.
    future_type next_future()
    {
      // reset and return another future
      if (_state->_future_acquired)
        _state->reset();
      return future_type(_state);
    }

    future_type get_return_object()
    {
      return future_type(_state);
    }

    std::experimental::suspend_never initial_suspend()
    {
      return {};
    }

    std::experimental::suspend_never final_suspend()
    {
      return {};
    }

    void return_value(const T& val)
    {
      _state->set_value(val);
    }
  };

  template <typename state_t>
  struct promise_t<void, state_t>
  {
    typedef future_t<void, state_t> future_type;
    std::shared_ptr<awaitable_state<void>> _state{ std::make_shared<awaitable_state<void>>() };

    // movable not copyable
    promise_t() = default;
    promise_t(const promise_t&) = delete;
    promise_t(promise_t&&) = default;

    void set_value()
    {
      _state->set_value();
    }

    future_type get_future()
    {
      return future_type(_state);
    }

    future_type get_return_object()
    {
      return future_type(_state);
    }

    std::experimental::suspend_never initial_suspend()
    {
      return {};
    }

    std::experimental::suspend_never final_suspend()
    {
      return {};
    }

    void return_void()
    {
      _state->set_value();
    }
  };

  // Simple RAII for uv_fs_t type
  struct fs_t : public ::uv_fs_t
  {
    ~fs_t()
    {
      ::uv_fs_req_cleanup(this);
    }
  };

  // Fixed size buffer
  template <size_t size>
  struct static_buf_t : ::uv_buf_t
  {
    char buffer[size];
    static_buf_t()
    {
      *(uv_buf_t*)this = uv_buf_init(buffer, sizeof(buffer));
    }
  };

  // Buffer based on null-terminated string
  struct string_buf_t : ::uv_buf_t
  {
    string_buf_t(const char* p)
    {
      *(uv_buf_t*)this = uv_buf_init(const_cast<char*>(p), strlen(p));
    }
  };

  // is_uv_handle_t checks for three data members: data, loop, and type.
  // These members mean this type is convertible to a uv_handle_t. This 
  // can be used to make it easier to call functions that take a handle.
  template <typename T, typename = int, typename = int, typename = int>
  struct is_uv_handle_t : std::false_type
  {
  };

  template <typename T>
  struct is_uv_handle_t<T, decltype((void)T::data, 0), decltype((void)T::loop, 0), decltype((void)T::type, 0)> : std::true_type
  {
  };

  template <typename T>
  auto unref(T* handle, typename std::enable_if<is_uv_handle_t<T>::value>::type* dummy = nullptr)
  {
    uv_unref(reinterpret_cast<uv_handle_t*>(handle));
  }

  template <typename T>
  auto ref(T* handle, typename std::enable_if<is_uv_handle_t<T>::value>::type* dummy = nullptr)
  {
    uv_ref(reinterpret_cast<uv_handle_t*>(handle));
  }

  auto fs_open(uv_loop_t* loop, uv_fs_t* req, const char* path, int flags, int mode)
  {
    promise_t<uv_file> awaitable;
    req->data = awaitable._state.get();

    auto ret = uv_fs_open(loop, req, path, flags, mode,
      [](uv_fs_t* req) -> void
    {
      static_cast<awaitable_state<uv_file>*>(req->data)->set_value(req->result);
    });

    if (ret != 0)
      awaitable.set_value(ret);
    return awaitable.get_future();
  }

  // return reference to passed in awaitable so that fs_open is directly awaitable
  auto& fs_open(awaitable_state<uv_file>& awaitable, uv_loop_t* loop, uv_fs_t* req, const char* path, int flags, int mode)
  {
    req->data = &awaitable;

    auto ret = uv_fs_open(loop, req, path, flags, mode,
      [](uv_fs_t* req) -> void
    {
      static_cast<awaitable_state<uv_file>*>(req->data)->set_value(req->result);
    });

    if (ret != 0)
      awaitable.set_value(ret);
    return awaitable;
  }

  auto fs_close(uv_loop_t* loop, uv_fs_t* req, uv_file file)
  {
    promise_t<int> awaitable;
    req->data = awaitable._state.get();

    auto ret = uv_fs_close(loop, req, file,
      [](uv_fs_t* req) -> void
    {
      static_cast<awaitable_state<int>*>(req->data)->set_value(req->result);
    });

    if (ret != 0)
      awaitable.set_value(ret);
    return awaitable.get_future();
  }

  auto& fs_close(awaitable_state<int>& awaitable, uv_loop_t* loop, uv_fs_t* req, uv_file file)
  {
    req->data = &awaitable;

    auto ret = uv_fs_close(loop, req, file,
      [](uv_fs_t* req) -> void
    {
      static_cast<awaitable_state<int>*>(req->data)->set_value(req->result);
    });

    if (ret != 0)
      awaitable.set_value(ret);
    return awaitable;
  }

  auto fs_write(uv_loop_t* loop, uv_fs_t* req, uv_file file, const uv_buf_t bufs[], unsigned int nbufs, int64_t offset)
  {
    promise_t<int> awaitable;
    req->data = awaitable._state.get();

    auto ret = uv_fs_write(loop, req, file, bufs, nbufs, offset,
      [](uv_fs_t* req) -> void
    {
      static_cast<awaitable_state<int>*>(req->data)->set_value(req->result);
    });

    if (ret != 0)
      awaitable.set_value(ret);
    return awaitable.get_future();
  }

  auto& fs_write(awaitable_state<int>& awaitable, uv_loop_t* loop, uv_fs_t* req, uv_file file, const uv_buf_t bufs[], unsigned int nbufs, int64_t offset)
  {
    req->data = &awaitable;

    auto ret = uv_fs_write(loop, req, file, bufs, nbufs, offset,
      [](uv_fs_t* req) -> void
    {
      static_cast<awaitable_state<int>*>(req->data)->set_value(req->result);
    });

    if (ret != 0)
      awaitable.set_value(ret);
    return awaitable;
  }

  auto fs_read(uv_loop_t* loop, uv_fs_t* req, uv_file file, const uv_buf_t bufs[], unsigned int nbufs, int64_t offset)
  {
    promise_t<int> awaitable;
    req->data = awaitable._state.get();

    auto ret = uv_fs_read(loop, req, file, bufs, nbufs, offset,
      [](uv_fs_t* req) -> void
    {
      static_cast<awaitable_state<int>*>(req->data)->set_value(req->result);
    });

    if (ret != 0)
      awaitable.set_value(ret);
    return awaitable.get_future();
  }

  auto& fs_read(awaitable_state<int>& awaitable, uv_loop_t* loop, uv_fs_t* req, uv_file file, const uv_buf_t bufs[], unsigned int nbufs, int64_t offset)
  {
    req->data = &awaitable;

    auto ret = uv_fs_read(loop, req, file, bufs, nbufs, offset,
      [](uv_fs_t* req) -> void
    {
      static_cast<awaitable_state<int>*>(req->data)->set_value(req->result);
    });

    if (ret != 0)
      awaitable.set_value(ret);
    return awaitable;
  }

  // generic stream functions
  auto write(::uv_write_t* req, uv_stream_t* handle, const uv_buf_t bufs[], unsigned int nbufs)
  {
    promise_t<int> awaitable;
    req->data = awaitable._state.get();

    auto ret = uv_write(req, handle, bufs, nbufs,
      [](uv_write_t* req, int status) -> void
    {
      static_cast<awaitable_state<int>*>(req->data)->set_value(status);
    });

    if (ret != 0)
      awaitable.set_value(ret);
    return awaitable.get_future();
  }

  // generic stream functions
  auto& write(awaitable_state<int>& awaitable, ::uv_write_t* req, uv_stream_t* handle, const uv_buf_t bufs[], unsigned int nbufs)
  {
    req->data = &awaitable;

    auto ret = uv_write(req, handle, bufs, nbufs,
      [](uv_write_t* req, int status) -> void
    {
      static_cast<awaitable_state<int>*>(req->data)->set_value(status);
    });

    if (ret != 0)
      awaitable.set_value(ret);
    return awaitable;
  }

  template <typename T>
  auto close(T* handle, typename std::enable_if<is_uv_handle_t<T>::value>::type* dummy = nullptr)
  {
    promise_t<void> awaitable;
    handle->data = awaitable._state.get();

    // uv_close returns void so no need to test return value
    uv_close(reinterpret_cast<uv_handle_t*>(handle),
      [](uv_handle_t* req) -> void
    {
      static_cast<awaitable_state<void>*>(req->data)->set_value();
    });
    return awaitable.get_future();
  }

  template <typename T>
  auto& close(awaitable_state<void>& awaitable, T* handle, typename std::enable_if<is_uv_handle_t<T>::value>::type* dummy = nullptr)
  {
    handle->data = &awaitable;

    // uv_close returns void so no need to test return value
    uv_close(reinterpret_cast<uv_handle_t*>(handle),
      [](uv_handle_t* req) -> void
    {
      static_cast<awaitable_state<void>*>(req->data)->set_value();
    });
    return awaitable;
  }

  // timer_start returns a promise, not a future.  This is because a timer can be repeating
  // You need to call next_future and await on it each time.  Note: if this is not a repeating
  // timer then calling it multiple times will cause the resumable function to "hang" (i.e.
  // never complete).
  auto timer_start(uv_timer_t* timer, uint64_t timeout, uint64_t repeat)
  {
    promise_t<int> awaitable;
    timer->data = awaitable._state.get();

    auto ret = ::uv_timer_start(timer,
      [](uv_timer_t* req) -> void
    {
      static_cast<awaitable_state<int>*>(req->data)->set_value(0);
    }, timeout, repeat);

    if (ret != 0)
      awaitable.set_value(ret);
    return awaitable;
  }

  struct timer_state_t : public awaitable_state<int>
  {
    timer_state_t& next()
    {
      reset();
      return *this;
    }
  };

  auto& timer_start(timer_state_t& awaitable, uv_timer_t* timer, uint64_t timeout, uint64_t repeat)
  {
    timer->data = &awaitable;

    auto ret = ::uv_timer_start(timer,
      [](uv_timer_t* req) -> void
    {
      static_cast<timer_state_t*>(req->data)->set_value(0);
    }, timeout, repeat);

    if (ret != 0)
      awaitable.set_value(ret);
    return awaitable;
  }

  auto tcp_connect(uv_connect_t* req, uv_tcp_t* socket, const struct sockaddr* dest)
  {
    promise_t<int> awaitable;
    req->data = awaitable._state.get();

    auto ret = ::uv_tcp_connect(req, socket, dest,
      [](uv_connect_t* req, int status) -> void
    {
      static_cast<awaitable_state<int>*>(req->data)->set_value(status);
    });

    if (ret != 0)
      awaitable.set_value(ret);
    return awaitable.get_future();
  }

  auto& tcp_connect(awaitable_state<int>& awaitable, uv_connect_t* req, uv_tcp_t* socket, const struct sockaddr* dest)
  {
    req->data = &awaitable;

    auto ret = ::uv_tcp_connect(req, socket, dest,
      [](uv_connect_t* req, int status) -> void
    {
      static_cast<awaitable_state<int>*>(req->data)->set_value(status);
    });

    if (ret != 0)
      awaitable.set_value(ret);
    return awaitable;
  }

  auto getaddrinfo(uv_loop_t* loop, uv_getaddrinfo_t* req, const char* node, const char* service, const struct addrinfo* hints)
  {
    // Create an extended state to hold the addrinfo that is passed in the callback
    struct addrinfo_state : public awaitable_state<int>
    {
      ::addrinfo* _addrinfo{ nullptr };
      ~addrinfo_state()
      {
        uv_freeaddrinfo(_addrinfo);
      }
      void set_value(int status, addrinfo* addrinfo)
      {
        _addrinfo = addrinfo;
        awaitable_state<int>::set_value(status);
      }
    };

    promise_t<int, addrinfo_state> awaitable;
    req->data = awaitable._state.get();

    auto ret = ::uv_getaddrinfo(loop, req,
      [](uv_getaddrinfo_t* req, int status, struct addrinfo* res) -> void
    {
      static_cast<addrinfo_state*>(req->data)->set_value(status, res);
    }, node, service, hints);

    if (ret != 0)
      awaitable._state->set_value(ret, nullptr);
    return awaitable.get_future();
  }

  // Create an extended state to hold the addrinfo that is passed in the callback
  struct addrinfo_state : public awaitable_state<int>
  {
    ::addrinfo* _addrinfo{ nullptr };
    ~addrinfo_state()
    {
      uv_freeaddrinfo(_addrinfo);
    }
    void set_value(int status, addrinfo* addrinfo)
    {
      _addrinfo = addrinfo;
      awaitable_state<int>::set_value(status);
    }
  };

  auto& getaddrinfo(addrinfo_state& awaitable, uv_loop_t* loop, uv_getaddrinfo_t* req, const char* node, const char* service, const struct addrinfo* hints)
  {
    req->data = &awaitable;

    auto ret = ::uv_getaddrinfo(loop, req,
      [](uv_getaddrinfo_t* req, int status, struct addrinfo* res) -> void
    {
      static_cast<addrinfo_state*>(req->data)->set_value(status, res);
    }, node, service, hints);

    if (ret != 0)
      awaitable.set_value(ret, nullptr);
    return awaitable;
  }

  // A read_buffer is allocated dynamically and lifetime is controlled through shared_ptr
  // in read_request_t and in the read_buffer::awaitable.  We may need to allocate a read_buffer
  // before the client code calls read_next. Once they do, however, lifetime is controlled
  // by the future.
  struct read_buffer : public awaitable_state<void>,
    public std::enable_shared_from_this<read_buffer>
  {
    uv_buf_t _buf = uv_buf_init(nullptr, 0);
    ssize_t _nread{ 0 };

    ~read_buffer()
    {
      if (_buf.base != nullptr)
        delete[] _buf.base;
    }

    void set_value(ssize_t nread, const uv_buf_t* p)
    {
      _buf = *p;
      _nread = nread;
      awaitable_state<void>::set_value();
    }

    // The result of awaiting on this will be std::shared_ptr<read_buffer>.
    std::shared_ptr<read_buffer> get_value()
    {
      return shared_from_this();
    }
  };

  // For reads, we need to define a new type to hold the completed read callbacks as we may not have
  // a future for them yet.  This is somewhat equivalent to other libuv functions that take a uv_write_t 
  // or a uv_fs_t.
  // This is a little convoluted as uv_read_start is not a one-shot read, but continues to provide
  // data to its callback.  So, we need to handle two cases.  One is where the future is created before
  // the data is passed to the callback and the second is where the future is not created first.
  class read_request_t
  {
    std::list<std::shared_ptr<read_buffer>> _buffers;
    typedef future_t<void, read_buffer> future_read;

    // We have data to provide.  If there is already a promise that has a future, then
    // use that.  Otherwise, we need to create a new promise for this new data.
    void add_buffer(ssize_t nread, const uv_buf_t* buf)
    {
      std::shared_ptr<read_buffer> promise;
      if (!_buffers.empty())
      {
        promise = _buffers.front();
        if (promise->_future_acquired)
        {
          _buffers.pop_front();
          promise->set_value(nread, buf);
          return;
        }
      }
      _buffers.emplace_back(std::make_shared<read_buffer>());
      _buffers.back()->set_value(nread, buf);
    }

  public:
    read_request_t() = default;
    // no copy/move
    read_request_t(const read_request_t&) = delete;
    read_request_t(read_request_t&&) = delete;

    // We may already have a promise with data available so check for that first.
    future_read read_next()
    {
      if (!_buffers.empty())
      {
        auto buffer = _buffers.front();
        if (!buffer->_future_acquired)
        {
          _buffers.pop_front();
          return future_read{ buffer };
        }
      }
      auto buffer = std::make_shared<read_buffer>();
      _buffers.push_back(buffer);
      return future_read{ buffer };
    }
    friend int read_start(uv_stream_t* handle, read_request_t* request);
  };

  // note: read_start does not return a future. All futures are acquired through read_request_t::read_next
  int read_start(uv_stream_t* handle, read_request_t* request)
  {
    uv_read_stop(handle);
    request->_buffers.clear();

    handle->data = request;

    int res = uv_read_start(handle,
      [](uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf)
    {
      *buf = uv_buf_init(new char[suggested_size], suggested_size);
    },
      [](uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf)
    {
      auto reader = reinterpret_cast<read_request_t*>(stream->data);
      reader->add_buffer(nread, buf);
    }
    );

    return res;
  }
} // namespace awaituv
