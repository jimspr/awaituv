# awaituv
Adapters for interfacing libuv with C++ resumable functions
Now uses C++20 coroutines.
MSVC - Recent MSVC compilers will work.
GCC - Requires GCC 11.4 or higher to avoid issue with co_await'ing a returned awaitable type reference that has a deleted move ctor.
Clang - Does not currently work.

You will need the following packages to build all of the samples, but the library itself only depends on libuv.
1. CMake
2. libuv
3. curl

For example, on Linux the following commands should get the relevant libraries.
sudo apt-get install cmake
sudo apt-get install libuv1-dev
sudo apt-get install libcurl4-openssl-dev
