// Test5.cpp : Defines the entry point for the console application.
//

#include <awaituv.h>
#include <fcntl.h>
#include <string>
#include <vector>

using namespace awaituv;
using namespace std;

future_t<void> start_http_google()
{
  uv_tcp_t socket;
  if (uv_tcp_init(uv_default_loop(), &socket) == 0) {
    // Use HTTP/1.0 rather than 1.1 so that socket is closed by server when
    // done sending data. Makes it easier than figuring it out on our end...
    const char* httpget =
        "GET / HTTP/1.0\r\n"
        "Host: www.google.com\r\n"
        "Cache-Control: max-age=0\r\n"
        "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,*/*;q=0.8\r\n"
        "\r\n";
    const char* host = "www.google.com";

    uv_getaddrinfo_t req;
    addrinfo_state   addrstate;
    if (co_await getaddrinfo(addrstate, uv_default_loop(), &req, host, "http",
                             nullptr) == 0) {
      uv_connect_t         connectreq;
      awaitable_state<int> connectstate;
      if (co_await tcp_connect(connectstate, &connectreq, &socket,
                               addrstate._addrinfo->ai_addr) == 0) {
        string_buf_t         buffer{ httpget };
        ::uv_write_t         writereq;
        awaitable_state<int> writestate;
        if (co_await write(writestate, &writereq, connectreq.handle, &buffer,
                           1) == 0) {
          read_request_t reader;
          if (read_start(connectreq.handle, &reader) == 0) {
            while (1) {
              auto state = co_await reader.read_next();
              if (state->_nread <= 0)
                break;
              uv_buf_t buf = uv_buf_init(state->_buf.base, state->_nread);
              fs_t     writereq;
              awaitable_state<int> writestate;
              (void)co_await fs_write(writestate, uv_default_loop(), &writereq,
                                      1 /*stdout*/, &buf, 1, -1);
            }
          }
        }
      }
    }
    awaitable_state<void> closestate;
    co_await close(closestate, &socket);
  }
}

future_t<void> test5()
{
  auto f1 = start_http_google();
  auto f2 = start_http_google();
  co_await future_of_any(f1, f2);
}

int main(int argc, char* argv[])
{
  test5();
  uv_run(uv_default_loop(), UV_RUN_DEFAULT);

  uv_loop_close(uv_default_loop());

  return 0;
}
