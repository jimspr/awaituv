// Test2.cpp : Defines the entry point for the console application.
//

#include "utils.h"
#include <awaituv.h>
#include <fcntl.h>
#include <string>
#include <vector>

using namespace awaituv;
using namespace std;

bool              run_timer = true;
uv_timer_t        color_timer;
awaitable_t<void> start_color_changer()
{
  static string_buf_t normal = "\033[40;37m";
  static string_buf_t red = "\033[41;37m";

  uv_timer_init(uv_default_loop(), &color_timer);

  uv_tty_t tty;
  uv_tty_init(uv_default_loop(), &tty, 1, 0);
  uv_tty_set_mode(&tty, UV_TTY_MODE_NORMAL);

  int cnt = 0;
  // unref the timer so that its existence won't keep
  // the loop alive
  unref(&color_timer);

  timer_state_t timerstate;
  uv_timer_start(timerstate, &color_timer, 1, 1);

  uv_write_t writereq;
  while (run_timer)
  {
    co_await timerstate.next();

    if (++cnt % 2 == 0)
    {
      awaitable_state<int> writestate;
      co_await uv_write(writestate, &writereq, reinterpret_cast<uv_stream_t*>(&tty), &normal, 1);
    }
    else
    {
      awaitable_state<int> writestate;
      co_await uv_write(writestate, &writereq, reinterpret_cast<uv_stream_t*>(&tty), &red, 1);
    }
  }

  // reset back to normal
  awaitable_state<int> writestate;
  co_await uv_write(writestate, &writereq, reinterpret_cast<uv_stream_t*>(&tty), &normal, 1);

  uv_tty_reset_mode();
  awaitable_state<void> closestate;
  co_await uv_close(closestate, &tty);
  closestate.reset();
  co_await uv_close(closestate, &color_timer); // close handle
}

void stop_color_changer()
{
  run_timer = false;
  // re-ref it so that loop won't exit until function above is done.
  ref(&color_timer);
}

awaitable_t<void> start_dump_file(const std::string& str)
{
  // We can use the same request object for all file operations as they don't overlap.
  static_buf_t<1024> buffer;

  uv_file file = co_await uv_fs_open(uv_default_loop(), str.c_str(), O_RDONLY, 0);
  if (file > 0)
  {
    while (1)
    {
      int result = co_await uv_fs_read(uv_default_loop(), file, &buffer, 1, -1);
      if (result <= 0)
        break;
      buffer.len = result;
      co_await uv_fs_write(uv_default_loop(), 1 /*stdout*/, &buffer, 1, -1);
    }
    co_await uv_fs_close(uv_default_loop(), file);
  }
}

awaitable_t<void> start_hello_world()
{
  for (int i = 0; i < 1000; ++i)
  {
    string_buf_t buf("\nhello world\n");
    co_await uv_fs_write(uv_default_loop(), 1 /*stdout*/, &buf, 1, -1);
  }
}

int main(int argc, char* argv[])
{
  // Process command line
  if (argc == 1)
  {
    printf("test2 [--noclor] [--sequential] <file1> <file2> ...");
    return -1;
  }

  bool fRunSequentially = false;
  bool fColorChanger = true;

  vector<string> files;
  for (int i = 1; i < argc; ++i)
  {
    string str = argv[i];
    if (str == "--sequential")
      fRunSequentially = true;
    else if (str == "--nocolor")
      fColorChanger = false;
    else
      files.push_back(str);
  }

  // start async color changer
  if (fColorChanger)
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
  if (fColorChanger)
    stop_color_changer();
  uv_run(uv_default_loop(), UV_RUN_DEFAULT);

  auto ret = uv_loop_close(uv_default_loop());
  assert(ret != UV_EBUSY);

  return 0;
}
