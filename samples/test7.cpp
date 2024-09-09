// Test1.cpp : Defines the entry point for the console application.
//

#include "utils.h"
#include <awaituv.h>

#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <iostream>

using namespace awaituv;
using namespace std;

void write_thread_id()
{
  static std::mutex mutex;
  std::lock_guard   lock(mutex);
  std::cout << std::this_thread::get_id() << std::endl;
}

/* In order to switch from some non-loop thread to the loop thread, we must use the uv_async_t object provided by
   libuv. It must be initialized (via uv_async_init) on the loop thread, but it can be scheduled (via uv_async_send) on
   any other thread. Additionally, we must not call uv_async_send until the coroutine actually calls co_await. When
   co_await is called, the await_suspend method of the awaiter is called with the coroutine_handle as a parameter. This
   provides the callback into the coroutine. We must have this address before calling uv_async_send. Note that because
   uv_async_init must be called from the loop thread, we can't delay creating/initing one until needed.

   Therefore, we either need a pool of these sitting around or we can use a single one for all callers. Ideally, the
   pool should grow/shrink as needed, but this would need to happen on the loop thread. For a single uv_async_t, we
   need a thread-safe collection of callbacks that we will iterate through when the loop thread calls back into the
   uv_async_t object. Even if uv_async_send is called multiple times for the same object, its callback will only be
   called once during a loop thread iteration. So, we provide a lock protecting the collection. A thread adding
   something will take the lock, add its callback to the collection, release the lock and call uv_async_send. When the
   callback occurs on the loop thread, the lock is taken, the collection is swapped/moved into a temp, the lock is
   released, and then each callback is made on the loop thread. These two approaches are roughly similar in complexity,
   but the second one seems a little better and avoids the possibility of a thread blocking until the loop thread can
   create more uv_async_t objects.
*/
struct loop_thread_switcher_t
{
  /* The uv_async_t memory must be freed in uv_close callback. */
  uv_async_t*                        _async = new uv_async_t{};
  std::mutex                         _mutex;
  std::vector<std::function<void()>> _callbacks;

  void add_callback(std::function<void()> func)
  {
    {
      std::lock_guard lock(_mutex);
      _callbacks.push_back(func);
    }
    ::uv_async_send(_async);
  }
  void execute_callbacks()
  {
    std::vector<std::function<void()>> callbacks;
    {
      std::lock_guard lock(_mutex);
      std::swap(callbacks, _callbacks);
    }
    for (auto& func : callbacks)
      func();
  }
  loop_thread_switcher_t(uv_loop_t* loop)
  {
    _async->data = this;
    uv_async_init(loop, _async, [](uv_async_t* req) {
      auto switcher = reinterpret_cast<loop_thread_switcher_t*>(req->data);
      switcher->execute_callbacks();
    });
    ::uv_unref(reinterpret_cast<uv_handle_t*>(_async));
  }
  ~loop_thread_switcher_t()
  {
    ::uv_close(reinterpret_cast<uv_handle_t*>(_async), [](uv_handle_t* req) {
      auto async = reinterpret_cast<uv_async_t*>(req);
      delete async;
    });
  }
};

struct loop_data_t
{
  std::unique_ptr<loop_thread_switcher_t> switcher;

  loop_data_t(uv_loop_t* loop)
  {
    switcher = std::make_unique<loop_thread_switcher_t>(loop);
    loop->data = this;
  }
};

struct switch_to_loop_thread_t
{
  uv_loop_t*              _loop;
  std::coroutine_handle<> _handle;

  switch_to_loop_thread_t(uv_loop_t* loop) : _loop(loop) {}

  bool await_ready()
  {
    return false;
  }
  void await_suspend(std::coroutine_handle<> h)
  {
    _handle = h;
    auto loop_data = reinterpret_cast<loop_data_t*>(_loop->data);
    // If loop_data is null, it means there is no loop_data_t object created for this loop.
    loop_data->switcher->add_callback([this]() -> void { callback(); });
  }
  void await_resume() {}
  void callback()
  {
    _handle.resume();
  }
};

struct switch_to_work_queue_t
{
  // The _work memory must remain valid until the loop thread callback is made.
  // This will be after this object is destroyed, so we will free it in the
  // callback.
  uv_work_t*              _work = new uv_work_t{};
  uv_loop_t*              _loop = nullptr;
  std::coroutine_handle<> _handle{};
  // Shortcut to see if await is already ready.
  bool await_ready()
  {
    return false;
  }
  void await_suspend(std::coroutine_handle<> h)
  {
    _handle = h;
    /* We have to run this lazily (i.e. after suspension) or the async could
       complete before the caller starts awaiting, which would NOT actually
       result in resuming on the loop thread. */
    uv_queue_work(
        _loop, _work,
        [](uv_work_t* req) {
          auto x = reinterpret_cast<switch_to_work_queue_t*>(req->data);
          auto handle = x->_handle;
          handle.resume();
        },
        [](uv_work_t* req, int status) { delete req; });
  }
  void await_resume() {}

  switch_to_work_queue_t(uv_loop_t* loop) : _loop(loop)
  {
    _work->data = this;
  }
};

awaitable_t<void> test_loop_data()
{
  write_thread_id();
  co_await switch_to_work_queue_t{ uv_default_loop() };
  write_thread_id();
  co_await switch_to_loop_thread_t{ uv_default_loop() };
  write_thread_id();
}

/* Here is another mechanism to switch threads. The local_thread_switcher_t must also be created on the loop thread,
   but it is not shared by multiple coroutines. As such, it doesn't need a lock or a collection of callbacks. It is
   meant to be used by a single coroutine that switches between threads as it runs. Typically, an instance of this
   object will be created at the top of a coroutine that starts on the loop thread. The coroutine can then switch
   between a worker thread and the loop thread by calling co_await obj.switch_to_worker_thread or co_await
   obj.switch_to_loop_thread. Example:

   task function()
   {
     local_thread_switcher_t switcher(loop);
     // Do some work on loop thread...
     co_await switcher.switch_to_worker_thread();
     // Do some work on the worker thread...
     co_await switcher.switch_to_loop_thread();
     // Do more work on loop thread.
   }
   Note that the function should finish on the loop thread or the original caller will end up on the wrong thread.
*/
struct local_thread_switcher_t
{
  uv_async_t*             _async = new uv_async_t{};
  uv_loop_t*              _loop = nullptr;
  std::coroutine_handle<> _handle;

  local_thread_switcher_t(uv_loop_t* loop) : _loop(loop)
  {
    _async->data = this;
    ::uv_async_init(loop, _async, [](uv_async_t* req) {
      auto switcher = reinterpret_cast<local_thread_switcher_t*>(req->data);
      switcher->_handle.resume();
    });
  }

  ~local_thread_switcher_t()
  {
    ::uv_close(reinterpret_cast<uv_handle_t*>(_async), [](uv_handle_t* req) {
      auto async = reinterpret_cast<uv_async_t*>(req);
      delete async;
    });
  }

  struct switch_to_loop_thread_t : public std::suspend_always
  {
    local_thread_switcher_t& _switcher;
    switch_to_loop_thread_t(local_thread_switcher_t& switcher) : _switcher(switcher) {}
    void await_suspend(std::coroutine_handle<> h)
    {
      _switcher.send_to_loop_thread(h);
    }
  };

  struct switch_to_worker_thread_t : public std::suspend_always
  {
    local_thread_switcher_t& _switcher;
    switch_to_worker_thread_t(local_thread_switcher_t& switcher) : _switcher(switcher) {}
    void await_suspend(std::coroutine_handle<> h)
    {
      _switcher.send_to_worker_thread(h);
    }
  };

  switch_to_loop_thread_t switch_to_loop_thread()
  {
    return switch_to_loop_thread_t{ *this };
  }

  switch_to_worker_thread_t switch_to_worker_thread()
  {
    return switch_to_worker_thread_t{ *this };
  }

  void send_to_loop_thread(std::coroutine_handle<> h)
  {
    _handle = h;
    ::uv_async_send(_async);
  }

  void send_to_worker_thread(std::coroutine_handle<> h)
  {
    // Manually allocate uv_work_t as it needs to live past the lifetime of this object.
    auto work = new uv_work_t;
    work->data = this;
    _handle = h;
    ::uv_queue_work(
        _loop, work,
        [](uv_work_t* req) {
          auto switcher = reinterpret_cast<local_thread_switcher_t*>(req->data);
          switcher->_handle.resume();
        },
        [](uv_work_t* req, int status) {
          // Manually delete the uv_work_t object.
          delete req;
        });
  }
};

awaitable_t<void> test_local_thread_switcher()
{
  local_thread_switcher_t switcher{ uv_default_loop() };
  write_thread_id();
  co_await switcher.switch_to_worker_thread();
  write_thread_id();
  co_await switcher.switch_to_loop_thread();
  write_thread_id();
}

int main(int argc, char* argv[])
{
  // Process command line
  if (argc != 1)
  {
    return -1;
  }

  // Create the shared loop_data_t object.
  {
    loop_data_t loop_data{ uv_default_loop() };
    std::cout << "test shared thread switcher mechanism" << std::endl;
    // The loop_data object will be shared by multiple coroutines/threads.
    test_loop_data();
    test_loop_data();
    test_loop_data();
    test_loop_data();
    test_loop_data();
    uv_run(uv_default_loop(), UV_RUN_DEFAULT);
  }

  std::cout << "test local thread switcher mechanism" << std::endl;
  test_local_thread_switcher();
  test_local_thread_switcher();
  test_local_thread_switcher();
  uv_run(uv_default_loop(), UV_RUN_DEFAULT);

  auto ret = uv_loop_close(uv_default_loop());
  assert(ret != UV_EBUSY);

  return 0;
}
