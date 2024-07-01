#pragma once
#include <awaituv.h>

inline awaituv::awaitable_t<size_t> start_http_google()
{
  size_t   count = 0;
  uv_tcp_t socket;
  if (uv_tcp_init(uv_default_loop(), &socket) == 0)
  {
    // Use HTTP/1.0 rather than 1.1 so that socket is closed by server when done sending data.
    // Makes it easier than figuring it out on our end...
    const char* httpget = "GET / HTTP/1.0\r\n"
                          "Host: www.google.com\r\n"
                          "Cache-Control: max-age=0\r\n"
                          "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,*/*;q=0.8\r\n"
                          "\r\n";
    const char* host = "www.google.com";

    awaituv::getaddrinfo_t req;
    auto                   state = co_await awaituv::uv_getaddrinfo(uv_default_loop(), &req, host, "http", nullptr);
    if (state == 0)
    {
      auto         info = req.addrinfo;
      uv_connect_t connectreq;
      auto         connect = co_await awaituv::uv_tcp_connect(&connectreq, &socket, info->ai_addr);
      if (connect == 0)
      {
        awaituv::string_buf_t buffer{ httpget };
        if (co_await awaituv::uv_write(connectreq.handle, &buffer, 1) == 0)
        {
          awaituv::read_request_t reader;
          if (reader.read_start(connectreq.handle) == 0)
          {
            while (1)
            {
              // awaituv::awaitable_state<awaituv::buffer_info> buffer;
              // auto info = co_await reader.read_next(buffer);
              auto info = co_await reader.read_next();
              if (info._nread <= 0)
                break;
              count += info._nread;
              uv_buf_t buf = uv_buf_init(info._buf.base, info._nread);
              (void)co_await awaituv::uv_fs_write(uv_default_loop(), 1 /*stdout*/, &buf, 1, -1);
            }
          }
        }
      }
    }
    co_await awaituv::uv_close(&socket);
  }
  co_return count;
}
