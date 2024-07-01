// Test4.cpp : Defines the entry point for the console application.
//

#include "utils.h"
#include <awaituv.h>
#include <fcntl.h>
#include <string>
#include <vector>

using namespace awaituv;
using namespace std;

awaitable_t<void> start_dump_file(const std::string str)
{
  // We can use the same request object for all file operations as they don't overlap.
  static_buf_t<1024> buffer;

  printf("opening %s\n", str.c_str());
  uv_file file = co_await uv_fs_open(uv_default_loop(), str.c_str(), O_RDONLY, 0);
  if (file > 0)
  {
    printf("opened %s\n", str.c_str());
    (void)co_await uv_fs_open(uv_default_loop(), file);
  }
}

int main(int argc, char* argv[])
{
  // Process command line
  if (argc == 1)
  {
    printf("test4 [--sequential] <file1> <file2> ...");
    return -1;
  }

  bool           fRunSequentially = false;
  vector<string> files;
  for (int i = 1; i < argc; ++i)
  {
    string str = argv[i];
    if (str == "--sequential")
      fRunSequentially = true;
    else
      files.push_back(str);
  }

  if (fRunSequentially)
    uv_run(uv_default_loop(), UV_RUN_DEFAULT);

  for (auto& file : files)
  {
    start_dump_file(file.c_str());
    if (fRunSequentially)
      uv_run(uv_default_loop(), UV_RUN_DEFAULT);
  }

  if (!fRunSequentially)
    uv_run(uv_default_loop(), UV_RUN_DEFAULT);

  uv_run(uv_default_loop(), UV_RUN_DEFAULT);

  uv_loop_close(uv_default_loop());

  return 0;
}
