#pragma once
#include <uv.h> // libuv
#include <functional>
#include <memory>
#include <list>
#include <string.h>
#include <string>
#include <atomic>
#include <tuple>
#include <vector>
#include <assert.h>

#if __has_include(<experimental/coroutine>)
#include <experimental/coroutine>
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

  void set_coroutine_callback(std::function<void(void)> cb)
  {
    // Test to make sure nothing else is waiting on this future.
    assert( ((cb == nullptr) || (_coro == nullptr)) && "This future is already being awaited.");
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
  bool await_ready() const
  {
    return _ready;
  }

  void await_suspend(std::experimental::coroutine_handle<> resume_cb)
  {
    set_coroutine_callback(resume_cb);
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

  void finalize_value()
  {
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
  auto await_resume() const
  {
    return _value;
  }
};

// specialization of awaitable_state<void>
template <>
struct awaitable_state<void> : public awaitable_state_base
{
  void get_value() const
  {
    if (!_ready)
      throw future_exception{ future_error::not_ready };
  }

  // make this directly awaitable
  void await_resume() const
  {
  }
};

// We need to be able to manage reference count of the state object in the callback.
template <typename awaitable_state_t>
struct counted_awaitable_state : public awaitable_state_t
{
  std::atomic<int> _count {0}; // tracks reference count of state object

  template <typename ...Args>
  counted_awaitable_state(Args&&... args) : _count{ 0 }, awaitable_state_t(std::forward<Args>(args)...)
  {
  }
  counted_awaitable_state(const counted_awaitable_state&) = delete;
  counted_awaitable_state(counted_awaitable_state&&) = delete;

  counted_awaitable_state* lock()
  {
    ++_count;
    return this;
  }

  void unlock()
  {
    if (--_count == 0)
      delete this;
  }
protected:
  ~counted_awaitable_state() {}
};

// counted_ptr is similar to shared_ptr but allows explicit control
//
template <typename T>
struct counted_ptr
{
  counted_ptr() = default;
  counted_ptr(const counted_ptr& cp) : _p(cp._p)
  {
    _lock();
  }

  counted_ptr(counted_awaitable_state<T>* p) : _p(p)
  {
    _lock();
  }

  counted_ptr(counted_ptr&& cp)
  {
    std::swap(_p, cp._p);
  }

  counted_ptr& operator=(const counted_ptr& cp)
  {
    if (&cp != this)
    {
      _unlock();
      _lock(cp._p);
    }
    return *this;
  }

  counted_ptr& operator=(counted_ptr&& cp)
  {
    if (&cp != this)
      std::swap(_p, cp._p);
    return *this;
  }

  ~counted_ptr()
  {
    _unlock();
  }

  counted_awaitable_state<T>* operator->() const
  {
    return _p;
  }

  counted_awaitable_state<T>* get() const
  {
    return _p;
  }

protected:
  void _unlock()
  {
    if (_p != nullptr)
    {
      auto t = _p;
      _p = nullptr;
      t->unlock();
    }
  }
  void _lock(counted_awaitable_state<T>* p)
  {
    if (p != nullptr)
      p->lock();
    _p = p;
  }
  void _lock()
  {
    if (_p != nullptr)
      _p->lock();
  }
  counted_awaitable_state<T>* _p = nullptr;
};

template <typename T, typename... Args>
counted_ptr<T> make_counted(Args&&... args)
{
  return new counted_awaitable_state<T>{ std::forward<Args>(args)... };
}

// The awaitable_state class is good enough for most cases, however there are some cases
// where a libuv callback returns more than one "value".  In that case, the function can
// define its own state type that holds more information.
template <typename T, typename state_t = awaitable_state<T>>
struct promise_t;

template <typename T, typename state_t = awaitable_state<T>>
struct future_t
{
  typedef T type;
  typedef promise_t<T, state_t> promise_type;
  counted_ptr<state_t> _state;

  future_t(const counted_ptr<state_t>& state) : _state(state)
  {
    _state->_future_acquired = true;
  }

  // movable, but not copyable
  future_t(const future_t&) = delete;
  future_t& operator=(const future_t&) = delete;
  future_t(future_t&& f) = default;
  future_t& operator=(future_t&&) = default;

  auto await_resume() const
  {
    return _state->get_value();
  }

  bool await_ready() const
  {
    return _state->_ready;
  }

  void await_suspend(std::experimental::coroutine_handle<> resume_cb)
  {
    _state->set_coroutine_callback(resume_cb);
  }

  bool ready() const
  {
    return _state->_ready;
  }

  auto get_value() const
  {
    return _state->get_value();
  }
};

template <typename T, typename state_t>
struct promise_t
{
  typedef future_t<T, state_t> future_type;
  typedef counted_awaitable_state<state_t> state_type;
  counted_ptr<state_t> _state;

  // movable not copyable
  template <typename ...Args>
  promise_t(Args&&... args) : _state(make_counted<state_t>(std::forward<Args>(args)...))
  {
  }
  promise_t(const promise_t&) = delete;
  promise_t(promise_t&&) = default;

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

  std::experimental::suspend_never initial_suspend() const
  {
    return {};
  }

  std::experimental::suspend_never final_suspend() const
  {
    return {};
  }

  void return_value(const T& val)
  {
    _state->set_value(val);
  }

  [[noreturn]] void unhandled_exception()
  {
    std::terminate();
  }
};

template <typename state_t>
struct promise_t<void, state_t>
{
  typedef future_t<void, state_t> future_type;
  typedef counted_awaitable_state<state_t> state_type;
  counted_ptr<state_t> _state;

  // movable not copyable
  template <typename ...Args>
  promise_t(Args&&... args) : _state(make_counted<state_t>(std::forward<Args>(args)...))
  {
  }
  promise_t(const promise_t&) = delete;
  promise_t(promise_t&&) = default;

  future_type get_future()
  {
    return future_type(_state);
  }

  future_type get_return_object()
  {
    return future_type(_state);
  }

  std::experimental::suspend_never initial_suspend() const
  {
    return {};
  }

  std::experimental::suspend_never final_suspend() const
  {
    return {};
  }

  void return_void()
  {
    _state->set_value();
  }

  [[noreturn]] void unhandled_exception()
  {
     std::terminate();
  }
};

// future_of_all is pretty trivial as we can just await on each argument

template <typename T>
future_t<void> future_of_all(T& f)
{
  co_await f;
}

template <typename T, typename... Rest>
future_t<void> future_of_all(T& f, Rest&... args)
{
  co_await f;
  co_await future_of_all(args...);
}

// future_of_all_range can take a vector/array of futures, although
// they must be of the same time. It returns a vector of all the results.
template <typename Iterator>
#ifdef _MSC_VER
auto future_of_all_range(Iterator begin, Iterator end) -> future_t<std::vector<decltype(begin->await_resume())>>
{
  std::vector<decltype(co_await *begin)> vec;
  while (begin != end)
  {
    vec.push_back(co_await *begin);
    ++begin;
  }
  co_return vec;
}
#else
auto future_of_all_range(Iterator begin, Iterator end) -> future_t<std::vector<typename decltype(*begin)::type>>
{
  std::vector<typename decltype(*begin)::type> vec;
  while (begin != end)
  {
    vec.push_back(co_await *begin);
    ++begin;
  }
  co_return vec;
}
#endif

// Define some helper templates to iterate through each element
// of the tuple
template <typename tuple_t, size_t N>
struct coro_helper_t
{
  static void set(tuple_t& tuple, std::function<void(void)> cb)
  {
    std::get<N>(tuple)._state->set_coroutine_callback(cb);
    coro_helper_t<tuple_t, N-1>::set(tuple, cb);
  }
};
// Specialization for last item
template <typename tuple_t>
struct coro_helper_t<tuple_t, 0>
{
  static void set(tuple_t& tuple, std::function<void(void)> cb)
  {
    std::get<0>(tuple)._state->set_coroutine_callback(cb);
  }
};

template <typename tuple_t>
void set_coro_helper(tuple_t& tuple, std::function<void(void)> cb)
{
  coro_helper_t<tuple_t, std::tuple_size<tuple_t>::value - 1>::set(tuple, cb);
}

// allows waiting for just one future to complete
template <typename... Rest>
struct multi_awaitable_state : public awaitable_state<void>
{
  // Store references to all the futures passed in.
  std::tuple<Rest&...> _futures;
  multi_awaitable_state(Rest&... args) : _futures(args...)
  {
  }

  void set_coroutine_callback(std::function<void(void)> cb)
  {
    set_coro_helper(_futures,
      [this]()
      {
        // reset callbacks on all futures to stop them
        set_coro_helper(_futures, nullptr);
        set_value();
      });
    awaitable_state<void>::set_coroutine_callback(cb);
  }
};

// future_of_any is pretty complicated
// We have to create a new promise with a custom awaitable state object
template <typename T, typename... Rest>
future_t<void, multi_awaitable_state<T, Rest...>> future_of_any(T& f, Rest&... args)
{
  promise_t<void, multi_awaitable_state<T, Rest...>> promise(f, args...);
  return promise.get_future();
}

// iterator_awaitable_state will track the index of which future completed
template <typename Iterator>
struct iterator_awaitable_state : public awaitable_state<Iterator>
{
  Iterator _begin;
  Iterator _end;
  iterator_awaitable_state(Iterator begin, Iterator end) : _begin(begin), _end(end)
  {
  }

  // any_completed will be called by any future completing
  void any_completed(Iterator completed)
  {
    // stop any other callbacks from coming in
    for (Iterator c = _begin; c != _end; ++c)
      c->_state->set_coroutine_callback(nullptr);
    set_value(completed);
  }

  void set_coroutine_callback(std::function<void(void)> cb)
  {
    for (Iterator c = _begin; c != _end; ++c)
    {
      std::function<void(void)> func = std::bind(&iterator_awaitable_state::any_completed, this, c);
      c->_state->set_coroutine_callback(func);
    }
    awaitable_state<Iterator>::set_coroutine_callback(cb);
  }
};

// returns the index of the iterator that succeeded
template <typename Iterator>
future_t<Iterator, iterator_awaitable_state<Iterator>> future_of_any_range(Iterator begin, Iterator end)
{
  promise_t<Iterator, iterator_awaitable_state<Iterator>> promise(begin, end);
  return promise.get_future();
}


template<typename T1, typename S1, typename T2, typename S2>
auto operator||(future_t<T1, S1>& t1, future_t<T2, S2>& t2)
{
  return future_of_any(t1, t2);
}

template<typename T1, typename S1, typename T2, typename S2>
auto operator&&(future_t<T1, S1>& t1, future_t<T2, S2>& t2)
{
  return future_of_all(t1, t2);
}

// Simple RAII for uv_loop_t type
class loop_t : public ::uv_loop_t
{
  int status = -1;
public:
  loop_t& operator=(const loop_t&) = delete; // no copy
  loop_t()
  {
    status = uv_loop_init(this);
    if (status != 0)
        throw std::exception();
  }
  ~loop_t()
  {
    if (status == 0)
      uv_loop_close(this);
  }
  int run()
  {
    return uv_run(this, UV_RUN_DEFAULT);
  }
  int run(uv_run_mode mode)
  {
    return uv_run(this, mode);
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
  string_buf_t(const char* p, size_t len)
  {
    *(uv_buf_t*)this = uv_buf_init(const_cast<char*>(p), len);
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

inline auto fs_open(uv_loop_t* loop, uv_fs_t* req, const char* path, int flags, int mode)
{
  promise_t<uv_file> awaitable;
  auto state = awaitable._state->lock();
  req->data = state;

  auto ret = uv_fs_open(loop, req, path, flags, mode,
    [](uv_fs_t* req) -> void
  {
    auto state = static_cast<promise_t<uv_file>::state_type*>(req->data);
    state->set_value(req->result);
    state->unlock();
  });

  if (ret != 0)
  {
    state->set_value(ret);
    state->unlock();
  }
  return awaitable.get_future();
}

// return reference to passed in awaitable so that fs_open is directly awaitable
inline auto& fs_open(awaitable_state<uv_file>& awaitable, uv_loop_t* loop, uv_fs_t* req, const char* path, int flags, int mode)
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

inline auto fs_close(uv_loop_t* loop, uv_fs_t* req, uv_file file)
{
  promise_t<int> awaitable;
  auto state = awaitable._state->lock();
  req->data = state;

  auto ret = uv_fs_close(loop, req, file,
    [](uv_fs_t* req) -> void
  {
    auto state = static_cast<promise_t<int>::state_type*>(req->data);
    state->set_value(req->result);
    state->unlock();
  });

  if (ret != 0)
  {
    state->set_value(ret);
    state->unlock();
  }
  return awaitable.get_future();
}

inline auto& fs_close(awaitable_state<int>& awaitable, uv_loop_t* loop, uv_fs_t* req, uv_file file)
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

inline auto fs_write(uv_loop_t* loop, uv_fs_t* req, uv_file file, const uv_buf_t bufs[], unsigned int nbufs, int64_t offset)
{
  promise_t<int> awaitable;
  auto state = awaitable._state->lock();
  req->data = state;

  auto ret = uv_fs_write(loop, req, file, bufs, nbufs, offset,
    [](uv_fs_t* req) -> void
  {
    auto state = static_cast<promise_t<int>::state_type*>(req->data);
    state->set_value(req->result);
    state->unlock();
  });

  if (ret != 0)
  {
    state->set_value(ret);
    state->unlock();
  }
  return awaitable.get_future();
}

inline auto& fs_write(awaitable_state<int>& awaitable, uv_loop_t* loop, uv_fs_t* req, uv_file file, const uv_buf_t bufs[], unsigned int nbufs, int64_t offset)
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

inline auto fs_read(uv_loop_t* loop, uv_fs_t* req, uv_file file, const uv_buf_t bufs[], unsigned int nbufs, int64_t offset)
{
  promise_t<int> awaitable;
  auto state = awaitable._state->lock();
  req->data = state;

  auto ret = uv_fs_read(loop, req, file, bufs, nbufs, offset,
    [](uv_fs_t* req) -> void
  {
    auto state = static_cast<promise_t<int>::state_type*>(req->data);
    state->set_value(req->result);
    state->unlock();
  });

  if (ret != 0)
  {
    state->set_value(ret);
    state->unlock();
  }
  return awaitable.get_future();
}

inline auto& fs_read(awaitable_state<int>& awaitable, uv_loop_t* loop, uv_fs_t* req, uv_file file, const uv_buf_t bufs[], unsigned int nbufs, int64_t offset)
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
inline auto write(::uv_write_t* req, uv_stream_t* handle, const uv_buf_t bufs[], unsigned int nbufs)
{
  promise_t<int> awaitable;
  auto state = awaitable._state->lock();
  req->data = state;

  auto ret = uv_write(req, handle, bufs, nbufs,
    [](uv_write_t* req, int status) -> void
  {
    auto state = static_cast<promise_t<int>::state_type*>(req->data);
    state->set_value(status);
    state->unlock();
  });

  if (ret != 0)
  {
    state->set_value(ret);
    state->unlock();
  }
  return awaitable.get_future();
}

// generic stream functions
inline auto& write(awaitable_state<int>& awaitable, ::uv_write_t* req, uv_stream_t* handle, const uv_buf_t bufs[], unsigned int nbufs)
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
  auto state = awaitable._state->lock();
  handle->data = state;

  // uv_close returns void so no need to test return value
  uv_close(reinterpret_cast<uv_handle_t*>(handle),
    [](uv_handle_t* req) -> void
  {
    auto state = static_cast<promise_t<void>::state_type*>(req->data);
    state->set_value();
    state->unlock();
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
inline auto timer_start(uv_timer_t* timer, uint64_t timeout, uint64_t repeat)
{
  promise_t<int> awaitable;
  auto state = awaitable._state.get();
  timer->data = state;

  auto ret = ::uv_timer_start(timer,
    [](uv_timer_t* req) -> void
  {
    auto state = static_cast<promise_t<int>::state_type*>(req->data);
    state->set_value(0);
  }, timeout, repeat);

  if (ret != 0)
    state->set_value(ret);
  return awaitable;
}

// This function does return a future as there is no repeat happening.
inline auto timer_start(uv_timer_t* timer, uint64_t timeout)
{
  promise_t<int> awaitable;
  auto state = awaitable._state.get();
  timer->data = state;

  auto ret = ::uv_timer_start(timer,
    [](uv_timer_t* req) -> void
  {
    auto state = static_cast<promise_t<int>::state_type*>(req->data);
    state->set_value(0);
  }, timeout, 0);

  if (ret != 0)
    state->set_value(ret);
  return awaitable.get_future();
}

struct timer_state_t : public awaitable_state<int>
{
  timer_state_t& next()
  {
    reset();
    return *this;
  }
};

inline auto& timer_start(timer_state_t& awaitable, uv_timer_t* timer, uint64_t timeout, uint64_t repeat)
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

inline auto tcp_connect(uv_connect_t* req, uv_tcp_t* socket, const struct sockaddr* dest)
{
  promise_t<int> awaitable;
  auto state = awaitable._state->lock();
  req->data = state;

  auto ret = ::uv_tcp_connect(req, socket, dest,
    [](uv_connect_t* req, int status) -> void
  {
    auto state = static_cast<promise_t<int>::state_type*>(req->data);
    state->set_value(status);
    state->unlock();
  });

  if (ret != 0)
  {
    state->set_value(ret);
    state->unlock();
  }
  return awaitable.get_future();
}

inline auto& tcp_connect(awaitable_state<int>& awaitable, uv_connect_t* req, uv_tcp_t* socket, const struct sockaddr* dest)
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

inline auto getaddrinfo(uv_loop_t* loop, uv_getaddrinfo_t* req, const char* node, const char* service, const struct addrinfo* hints)
{
  promise_t<int, addrinfo_state> awaitable;
  auto state = awaitable._state->lock();
  req->data = state;

  auto ret = ::uv_getaddrinfo(loop, req,
    [](uv_getaddrinfo_t* req, int status, struct addrinfo* res) -> void
  {
    auto state = static_cast<promise_t<int, addrinfo_state>::state_type*>(req->data);
    state->set_value(status, res);
    state->unlock();
  }, node, service, hints);

  if (ret != 0)
  {
    state->set_value(ret, nullptr);
    state->unlock();
  }
  return awaitable.get_future();
}

inline auto& getaddrinfo(addrinfo_state& awaitable, uv_loop_t* loop, uv_getaddrinfo_t* req, const char* node, const char* service, const struct addrinfo* hints)
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

// A read_buffer is allocated dynamically and lifetime is controlled through counted_ptr
// in read_request_t and in the read_buffer::awaitable.  We may need to allocate a read_buffer
// before the client code calls read_next. Once they do, however, lifetime is controlled
// by the future.
struct read_buffer : public awaitable_state<void>
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

  // The result of awaiting on this will be std::counted_ptr<read_buffer>.
  counted_ptr<read_buffer> get_value()
  {
    return static_cast<counted_awaitable_state<read_buffer>*>(this);
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
  std::list<counted_ptr<read_buffer>> _buffers;
  typedef future_t<void, read_buffer> future_read;

  // We have data to provide.  If there is already a promise that has a future, then
  // use that.  Otherwise, we need to create a new promise for this new data.
  void add_buffer(ssize_t nread, const uv_buf_t* buf)
  {
    counted_ptr<read_buffer> promise;
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
    _buffers.emplace_back(make_counted<read_buffer>());
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
    auto buffer = make_counted<read_buffer>();
    _buffers.push_back(buffer);
    return future_read{ buffer };
  }
  friend int read_start(uv_stream_t* handle, read_request_t* request);
};

// note: read_start does not return a future. All futures are acquired through read_request_t::read_next
inline int read_start(uv_stream_t* handle, read_request_t* request)
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

inline future_t<std::string> stream_to_string(uv_stream_t* handle)
{
  read_request_t reader;
  std::string str;
  if (read_start(handle, &reader) == 0)
  {
    while (1)
    {
      auto state = co_await reader.read_next();
      if (state->_nread <= 0)
        break;
      str.append(state->_buf.base, state->_nread);
    }
  }
  co_return str;
}
} // namespace awaituv
