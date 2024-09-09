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
class curl_context_t
{
public:
  uv_poll_t*        _poll_handle = new uv_poll_t{};
  curl_socket_t     _socket;
  curl_requester_t* _requester;
  curl_context_t(uv_loop_t& loop, curl_socket_t socket, curl_requester_t* requester)
    : _socket(socket), _requester(requester)
  {
    uv_poll_init_socket(&loop, _poll_handle, socket);
    _poll_handle->data = this;
  }
  ~curl_context_t()
  {
    ::uv_close((uv_handle_t*)_poll_handle, [](uv_handle_t* req) {
      auto poll = reinterpret_cast<uv_poll_t*>(req);
      delete poll;
    });
  }
};

// Manages a "multi handle".
class curl_requester_t
{
public:
  // create some typedefs to make casting lambdas to correct function pointers
  // easy. curl_multi_setopt is a vararg function and passing a lambda directly
  // does not work.
  typedef size_t (*write_callback)(char* ptr, size_t size, size_t nmemb, void* userdata);
  typedef size_t (*header_callback)(char* ptr, size_t size, size_t nmemb, void* userdata);
  typedef int    (*socket_callback)(CURL* easy, curl_socket_t s, int action, void* userp, void* socketp);
  typedef int    (*timer_callback)(CURLM* multi, long timeout_ms, void* userp);

  uv_loop_t&  _loop;
  CURLM*      _multi_handle;
  uv_timer_t* _timeout = new uv_timer_t{};
  bool        _verbose = false;

  curl_requester_t(uv_loop_t& loop) : _loop(loop)
  {
    uv_timer_init(&loop, _timeout);
    _timeout->data = this;

    _multi_handle = curl_multi_init();
    if (_multi_handle == nullptr)
      throw std::bad_alloc();
    curl_multi_setopt(_multi_handle, CURLMOPT_SOCKETDATA, this);
    curl_multi_setopt(
        _multi_handle, CURLMOPT_SOCKETFUNCTION,
        (socket_callback)[](CURL * easy, curl_socket_t s, int action, void* userp, void* socketp)->int {
          auto requester = static_cast<curl_requester_t*>(userp);
          return requester->socket_function(easy, s, action, socketp);
        });
    curl_multi_setopt(_multi_handle, CURLMOPT_TIMERDATA, this);
    curl_multi_setopt(
        _multi_handle, CURLMOPT_TIMERFUNCTION, (timer_callback)[](CURLM * multi, long timeout_ms, void* userp)->int {
          auto requester = static_cast<curl_requester_t*>(userp);
          return requester->timer_function(multi, timeout_ms);
        });
  }

  ~curl_requester_t()
  {
    uv_close((uv_handle_t*)_timeout, [](uv_handle_t* handle) -> void {
      auto timer = reinterpret_cast<uv_timer_t*>(handle);
      delete timer;
    });
    curl_multi_cleanup(_multi_handle);
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
        context = new curl_context_t(_loop, s, this);
      int events = 0;

      curl_multi_assign(_multi_handle, s, context);

      if (action != CURL_POLL_IN)
        events |= UV_WRITABLE;
      if (action != CURL_POLL_OUT)
        events |= UV_READABLE;

      uv_poll_start(context->_poll_handle, events, [](uv_poll_t* req, int status, int events) -> void {
        auto requester = static_cast<curl_context_t*>(req->data)->_requester;
        requester->handle_events(req, status, events);
      });
    }
    break;
    case CURL_POLL_REMOVE:
      if (context != nullptr)
      {
        uv_poll_stop(context->_poll_handle);
        // poll_handle will be closed in context destructor
        delete context;

        curl_multi_assign(_multi_handle, s, NULL);
      }
      break;
    }

    return 0;
  }

  // handle poll events
  void handle_events(uv_poll_t* req, int status, int events)
  {
    uv_timer_stop(_timeout);
    auto context = static_cast<curl_context_t*>(req->data);

    int mask = 0;
    if (events & UV_READABLE)
      mask |= CURL_CSELECT_IN;
    if (events & UV_WRITABLE)
      mask |= CURL_CSELECT_OUT;

    int running_handles;
    curl_multi_socket_action(_multi_handle, context->_socket, mask, &running_handles);

    process_messages();
  }

  void process_messages()
  {
    CURLMsg* message;
    int      pending;

    while ((message = curl_multi_info_read(_multi_handle, &pending)))
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
        curl_multi_remove_handle(_multi_handle, handle);
        state->set_value(); // directly set individual parts, no need to pass
                            // whole response
      }
    }
  }

  // This is called for CURLMOPT_TIMERFUNCTION
  int timer_function(CURLM* multi, long timeout_ms)
  {
    if (timeout_ms == -1) // delete timer
      uv_timer_stop(_timeout);
    else
    {
      uv_timer_start(
          _timeout,
          [](uv_timer_t* req) -> void {
            auto requester = static_cast<curl_requester_t*>(req->data);
            int  running_handles;
            curl_multi_socket_action(requester->_multi_handle, CURL_SOCKET_TIMEOUT, 0, &running_handles);
            requester->process_messages();
          },
          timeout_ms, 0);
    }
    return 0;
  }

  awaitable_state<http_response_t>& invoke(awaitable_state<http_response_t>& awaitable, CURL* handle)
  {
    auto state = &awaitable;

    if (_verbose)
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
    curl_multi_add_handle(_multi_handle, handle);

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
