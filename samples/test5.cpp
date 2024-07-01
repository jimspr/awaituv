// Test5.cpp : Defines the entry point for the console application.
//

#include "utils.h"
#include <awaituv.h>
#include <fcntl.h>
#include <string>
#include <vector>

using namespace awaituv;
using namespace std;

awaitable_t<void> test_a()
{
  auto f1 = start_http_google();
  auto f2 = start_http_google();
  co_await future_of_all(f1, f2);
}

awaitable_t<void> test_b()
{
  awaitable_t<size_t> futures[2] = { start_http_google(), start_http_google() };
  auto             fut = future_of_all_range(&futures[0], &futures[2]);
  auto             results = co_await fut;
  printf("\n");
  for (auto& i : results)
    printf("%zu\n", i);
}

awaitable_t<void> test_c()
{
  std::vector<awaitable_t<size_t>> vec;
  vec.push_back(start_http_google());
  vec.push_back(start_http_google());
  auto fut = future_of_all_range(vec.begin(), vec.end());
  auto results = co_await fut;
  printf("\n");
  for (auto& i : results)
    printf("%zu\n", i);
}

awaitable_t<void> test_d()
{
  auto f1 = start_http_google();
  auto f2 = start_http_google();
  co_await future_of_any(f1, f2);
}

awaitable_t<void> test_e()
{
  auto f1 = uv_fs_open(uv_default_loop(), "e:\\awaituv2\\inc\\awaituv.h", O_RDONLY, 0);
  auto f2 = uv_fs_open(uv_default_loop(), "e:\\awaituv2\\inc\\awaituv.h", O_RDONLY, 0);
  co_await future_of_any(f1, f2);
  int file;
  if (f1._ready)
    file = f1._value;
  else if (f2._ready)
    file = f2._value;
  printf("file = %d\n", file);
}

int main(int argc, char* argv[])
{
  // Process command line
  if (argc != 1)
  {
    printf("test5");
    return -1;
  }

  printf("test_a--------------------------------------------------\n");
  test_a();
  uv_run(uv_default_loop(), UV_RUN_DEFAULT);

  printf("test_b--------------------------------------------------\n");
  test_b();
  uv_run(uv_default_loop(), UV_RUN_DEFAULT);

  printf("test_c--------------------------------------------------\n");
  test_c();
  uv_run(uv_default_loop(), UV_RUN_DEFAULT);

  printf("test_d--------------------------------------------------\n");
  test_d();
  uv_run(uv_default_loop(), UV_RUN_DEFAULT);

  printf("test_e--------------------------------------------------\n");
  test_e();
  uv_run(uv_default_loop(), UV_RUN_DEFAULT);

  uv_loop_close(uv_default_loop());

  return 0;
}
