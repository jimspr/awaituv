// Test6.cpp : Defines the entry point for the console application.
//

#include <awaituv.h>

using namespace awaituv;
using namespace std;

awaitable_t<void> test6()
{
  co_return;
}

awaitable_t<void> forward(awaitable_t<void> f)
{
  co_await f;
}

int main(int argc, char* argv[])
{
  auto future = test6();
  //  future = test6();
  forward(std::move(future));
  forward(test6());
  return 0;
}
