// TestUV.cpp : Defines the entry point for the console application.
//

#include <awaituv.h>

using namespace awaituv;
using namespace std;

future_t<void> test3()
{
  co_return;
}

future_t<void> forward(future_t<void> f)
{
  co_await f;
}

int main(int argc, char* argv[])
{
  auto future = test3();
  future = test3();
  forward(std::move(future));
  forward(test3());
  return 0;
}
