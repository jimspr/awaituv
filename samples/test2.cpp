// TestUV.cpp : Defines the entry point for the console application.
//

#include <vector>
#include <string>
#include <fcntl.h>
#include <awaituv.h>

using namespace awaituv;
using namespace std;

bool run_timer = true;
uv_timer_t color_timer;
future_t<void> start_color_changer()
{
  static string_buf_t normal = "\033[40;37m";
  static string_buf_t red = "\033[41;37m";

  uv_timer_init(uv_default_loop(), &color_timer);

  uv_write_t writereq;
  uv_tty_t tty;
  uv_tty_init(uv_default_loop(), &tty, 1, 0);
  uv_tty_set_mode(&tty, UV_TTY_MODE_NORMAL);

  int cnt = 0;
  unref(&color_timer);

  timer_state_t timerstate;
  timer_start(timerstate, &color_timer, 1, 1);

  while (run_timer)
  {
    (void) co_await timerstate.next();

    if (++cnt % 2 == 0)
    {
      awaitable_state<int> writestate;
      (void) co_await write(writestate, &writereq, reinterpret_cast<uv_stream_t*>(&tty), &normal, 1);
    }
    else
    {
      awaitable_state<int> writestate;
      (void) co_await write(writestate, &writereq, reinterpret_cast<uv_stream_t*>(&tty), &red, 1);
    }
  }

  //reset back to normal
  awaitable_state<int> writestate;
  (void) co_await write(writestate, &writereq, reinterpret_cast<uv_stream_t*>(&tty), &normal, 1);

  uv_tty_reset_mode();
  awaitable_state<void> closestate;
  co_await close(closestate, &tty);
  closestate.reset();
  co_await close(closestate, &color_timer); // close handle
}

void stop_color_changer()
{
  run_timer = false;
  // re-ref it so that loop won't exit until function above is done.
  ref(&color_timer);
}

future_t<void> start_http_google()
{
  uv_tcp_t socket;
  if (uv_tcp_init(uv_default_loop(), &socket) == 0)
  {
    // Use HTTP/1.0 rather than 1.1 so that socket is closed by server when done sending data.
    // Makes it easier than figuring it out on our end...
    const char* httpget =
      "GET / HTTP/1.0\r\n"
      "Host: www.google.com\r\n"
      "Cache-Control: max-age=0\r\n"
      "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,*/*;q=0.8\r\n"
      "\r\n";
    const char* host = "www.google.com";

    uv_getaddrinfo_t req;
    addrinfo_state addrstate;
    if (co_await getaddrinfo(addrstate, uv_default_loop(), &req, host, "http", nullptr) == 0)
    {
      uv_connect_t connectreq;
      awaitable_state<int> connectstate;
      if (co_await tcp_connect(connectstate, &connectreq, &socket, addrstate._addrinfo->ai_addr) == 0)
      {
        string_buf_t buffer{ httpget };
        ::uv_write_t writereq;
        awaitable_state<int> writestate;
        if (co_await write(writestate, &writereq, connectreq.handle, &buffer, 1) == 0)
        {
          read_request_t reader;
          if (read_start(connectreq.handle, &reader) == 0)
          {
            while (1)
            {
              auto state = co_await reader.read_next();
              if (state->_nread <= 0)
                break;
              uv_buf_t buf = uv_buf_init(state->_buf.base, state->_nread);
              fs_t writereq;
              awaitable_state<int> writestate;
              (void) co_await fs_write(writestate, uv_default_loop(), &writereq, 1 /*stdout*/, &buf, 1, -1);
            }
          }
        }
      }
    }
    awaitable_state<void> closestate;
    co_await close(closestate, &socket);
  }
}

future_t<void> start_dump_file(const std::string& str)
{
  // We can use the same request object for all file operations as they don't overlap.
  static_buf_t<1024> buffer;

  //fs_t openreq;
  //awaitable_state<uv_file> state;
  awaitable_fs_open awaitable;
  uv_file file = co_await fs_open(uv_default_loop(), &awaitable, str.c_str(), O_RDONLY, 0);
  if (file > 0)
  {
    while (1)
    {
      fs_t readreq;
      awaitable_state<int> readstate;
      int result = co_await fs_read(readstate, uv_default_loop(), &readreq, file, &buffer, 1, -1);
      if (result <= 0)
        break;
      buffer.len = result;
      fs_t req;
      awaitable_state<int> writestate;
      (void) co_await fs_write(writestate, uv_default_loop(), &req, 1 /*stdout*/, &buffer, 1, -1);
    }
    fs_t closereq;
    awaitable_state<int> closestate;
    (void) co_await fs_close(closestate, uv_default_loop(), &closereq, file);
  }
}

future_t<void> start_hello_world()
{
  for (int i = 0; i < 1000; ++i)
  {
    string_buf_t buf("\nhello world\n");
    fs_t req;
    awaitable_state<int> writestate;
    (void) co_await fs_write(writestate, uv_default_loop(), &req, 1 /*stdout*/, &buf, 1, -1);
  }
}

int main(int argc, char* argv[])
{
  // Process command line
  if (argc == 1)
  {
    printf("testuv [--sequential] <file1> <file2> ...");
    return -1;
  }

  bool fRunSequentially = false;
  vector<string> files;
  for (int i = 1; i < argc; ++i)
  {
    string str = argv[i];
    if (str == "--sequential")
      fRunSequentially = true;
    else
      files.push_back(str);
  }

  // start async color changer
  start_color_changer();

  start_hello_world();
  if (fRunSequentially)
    uv_run(uv_default_loop(), UV_RUN_DEFAULT);

  for (auto& file : files)
  {
    start_dump_file(file.c_str());
    if (fRunSequentially)
      uv_run(uv_default_loop(), UV_RUN_DEFAULT);
  }

  start_http_google();
  if (fRunSequentially)
    uv_run(uv_default_loop(), UV_RUN_DEFAULT);

  if (!fRunSequentially)
    uv_run(uv_default_loop(), UV_RUN_DEFAULT);

  // stop the color changer and let it get cleaned up
  stop_color_changer();
  uv_run(uv_default_loop(), UV_RUN_DEFAULT);

  uv_loop_close(uv_default_loop());

  return 0;
}
