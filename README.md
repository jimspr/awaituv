# awaituv
Adapters for interfacing libuv with C++ resumable functions
Now uses C++20 coroutines.
MSVC - Most recent MSVC compilers will work.
GCC - Requires GCC 11.4 to avoid issue with co_await'ing a returned awaitable type reference that has a deleted move ctor.
Clang - Does not currently work.

You will need the following packages to build all of the samples, but the library itself only depends on libuv.
1. libuv
2. curl
