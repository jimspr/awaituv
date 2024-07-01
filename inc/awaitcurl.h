#pragma once
#include <awaituv.h>
#include <curl/curl.h>
#include <stdio.h>
#include <string>
#include <vector>

namespace awaitcurl
{
using namespace awaituv;

// Provides a RAII type for CURL global initialization/cleanup
struct curl_global_t
{
  curl_global_t& operator=(const curl_global_t&) = delete; // no copy
  curl_global_t()
  {
    auto result = curl_global_init(CURL_GLOBAL_ALL);
    if (result)
      throw result;
  }
  curl_global_t(long flags)
  {
    auto result = curl_global_init(flags);
    if (result)
      throw result;
  }
  ~curl_global_t()
  {
    curl_global_cleanup();
  }
};

// Basic http response type
struct http_response_t
{
  long                     http_code{ 0 };
  CURLcode                 curl_code{ CURLE_OK };
  std::string              str;
  std::vector<std::string> headers;

  bool is_success()
  {
    return (http_code >= 200) && (http_code <= 299);
  }

  void print_response(const char* msg)
  {
    printf("-----------------------------------------------------------------------------------\n");
    printf("%s: http:%ld curl:%d-\"%s\"\n%s\n", msg, http_code, curl_code, curl_easy_strerror(curl_code), str.c_str());
  }
};

struct curl_requester_t;
struct curl_context_t
{
  uv_poll_t         poll_handle;
  curl_socket_t     socket;
  curl_requester_t* requester;
  curl_context_t(uv_loop_t& loop, curl_socket_t socket, curl_requester_t* requester)
    : socket(socket), requester(requester)
  {
    uv_poll_init_socket(&loop, &poll_handle, socket);
    poll_handle.data = this;
  }
  ~curl_context_t() {}
};

// Manages a "multi handle".
struct curl_requester_t
{
  // create some typedefs to make casting lambdas to correct function pointers
  // easy. curl_multi_setopt is a vararg function and passing a lambda directly
  // does not work.
  typedef size_t (*write_callback)(char* ptr, size_t size, size_t nmemb, void* userdata);
  typedef size_t (*header_callback)(char* ptr, size_t size, size_t nmemb, void* userdata);
  typedef int (*socket_callback)(CURL* easy, curl_socket_t s, int action, void* userp, void* socketp);
  typedef int (*timer_callback)(CURLM* multi, long timeout_ms, void* userp);

  uv_loop_t& loop;
  CURLM*     multi_handle;
  uv_timer_t timeout;
  bool       verbose = false;

  curl_requester_t(uv_loop_t& loop) : loop(loop)
  {
    uv_timer_init(&loop, &timeout);
    timeout.data = this;

    multi_handle = curl_multi_init();
    if (multi_handle == nullptr)
      throw std::bad_alloc();
    curl_multi_setopt(multi_handle, CURLMOPT_SOCKETDATA, this);
    curl_multi_setopt(
        multi_handle, CURLMOPT_SOCKETFUNCTION,
        (socket_callback)[](CURL * easy, curl_socket_t s, int action, void* userp, void* socketp)->int {
          auto requester = static_cast<curl_requester_t*>(userp);
          return requester->socket_function(easy, s, action, socketp);
        });
    curl_multi_setopt(multi_handle, CURLMOPT_TIMERDATA, this);
    curl_multi_setopt(
        multi_handle, CURLMOPT_TIMERFUNCTION, (timer_callback)[](CURLM * multi, long timeout_ms, void* userp)->int {
          auto requester = static_cast<curl_requester_t*>(userp);
          return requester->timer_function(multi, timeout_ms);
        });
  }

  ~curl_requester_t()
  {
    curl_multi_cleanup(multi_handle);
  }

  int socket_function(CURL* easy, curl_socket_t s, int action, void* socketp)
  {
    curl_context_t* context = static_cast<curl_context_t*>(socketp);
    switch (action)
    {
    case CURL_POLL_IN:
    case CURL_POLL_OUT:
    case CURL_POLL_INOUT: {
      // create a context if this is the first time
      if (context == nullptr)
        context = new curl_context_t(loop, s, this);
      int events = 0;

      curl_multi_assign(multi_handle, s, context);

      if (action != CURL_POLL_IN)
        events |= UV_WRITABLE;
      if (action != CURL_POLL_OUT)
        events |= UV_READABLE;

      uv_poll_start(&context->poll_handle, events, [](uv_poll_t* req, int status, int events) -> void {
        auto requester = static_cast<curl_context_t*>(req->data)->requester;
        requester->handle_events(req, status, events);
      });
    }
    break;
    case CURL_POLL_REMOVE:
      if (context != nullptr)
      {
        uv_poll_stop(&context->poll_handle);
        uv_close((uv_handle_t*)&context->poll_handle, [](uv_handle_t* handle) -> void {
          auto context = static_cast<curl_context_t*>(handle->data);
          delete context;
        });

        curl_multi_assign(multi_handle, s, NULL);
      }
      break;
    }

    return 0;
  }

  // handle poll events
  void handle_events(uv_poll_t* req, int status, int events)
  {
    uv_timer_stop(&timeout);
    auto context = static_cast<curl_context_t*>(req->data);

    int mask = 0;
    if (events & UV_READABLE)
      mask |= CURL_CSELECT_IN;
    if (events & UV_WRITABLE)
      mask |= CURL_CSELECT_OUT;

    int running_handles;
    curl_multi_socket_action(multi_handle, context->socket, mask, &running_handles);

    process_messages();
  }

  void process_messages()
  {
    CURLMsg* message;
    int      pending;

    while ((message = curl_multi_info_read(multi_handle, &pending)))
    {
      if (message->msg == CURLMSG_DONE)
      {
        CURL* handle = message->easy_handle;

        awaitable_state<http_response_t>* state;
        curl_easy_getinfo(handle, CURLINFO_PRIVATE, &state);
        state->_value.curl_code = message->data.result;

        curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &state->_value.http_code);
        // set_value will resume the coroutine and the easy handle could be
        // released so remove it now.
        curl_multi_remove_handle(multi_handle, handle);
        state->set_value(); // directly set individual parts, no need to pass
                            // whole response
      }
    }
  }

  // This is called for CURLMOPT_TIMERFUNCTION
  int timer_function(CURLM* multi, long timeout_ms)
  {
    if (timeout_ms == -1) // delete timer
      uv_timer_stop(&timeout);
    else
    {
      uv_timer_start(
          &timeout,
          [](uv_timer_t* req) -> void {
            auto requester = static_cast<curl_requester_t*>(req->data);
            int  running_handles;
            curl_multi_socket_action(requester->multi_handle, CURL_SOCKET_TIMEOUT, 0, &running_handles);
            requester->process_messages();
          },
          timeout_ms, 0);
    }
    return 0;
  }

  awaitable_state<http_response_t>& invoke(awaitable_state<http_response_t>& awaitable, CURL* handle)
  {
    auto state = &awaitable;

    if (verbose)
      curl_easy_setopt(handle, CURLOPT_VERBOSE, 1);

    curl_easy_setopt(
        handle, CURLOPT_WRITEFUNCTION,
        (write_callback)[](char* buffer, size_t size, size_t nmemb, void* userp)->size_t {
          auto state = static_cast<awaitable_state<http_response_t>*>(userp);
          state->_value.str += std::string(buffer, buffer + size * nmemb);
          return size * nmemb;
        });
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, state);

    curl_easy_setopt(
        handle, CURLOPT_HEADERFUNCTION,
        (header_callback)[](char* buffer, size_t size, size_t nmemb, void* userp)->size_t {
          auto state = static_cast<awaitable_state<http_response_t>*>(userp);
          state->_value.headers.push_back(std::string(buffer, buffer + size * nmemb));
          return size * nmemb;
        });
    curl_easy_setopt(handle, CURLOPT_HEADERDATA, state);

    curl_easy_setopt(handle, CURLOPT_PRIVATE, state);
    curl_multi_add_handle(multi_handle, handle);

    return awaitable;
  }

  awaitable_t<http_response_t> invoke(CURL* handle)
  {
    awaitable_state<http_response_t> state;
    co_return co_await invoke(state, handle);
  }

  awaitable_t<http_response_t> invoke(const char* url)
  {
    auto handle = curl_easy_init();
    curl_easy_setopt(handle, CURLOPT_URL, url);
    curl_easy_setopt(handle, CURLOPT_HTTPGET, 1L);

    // await the invoke so that the handle can be cleaned up after it's done
    auto response = co_await this->invoke(handle);
    curl_easy_cleanup(handle);
    co_return response;
  }
};

} // namespace awaitcurl
