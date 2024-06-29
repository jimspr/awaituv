# awaituv
Adapters for interfacing libuv with C++ resumable functions
Requires GCC 11.4 to avoid issue with co_await'ing a returned awaitable type reference that has a deleted move ctor.

You will need the following packages to build all of the samples, but the library itself only depends on libuv.
1. libuv
2. curl
3. libssh2
4. openssl
5. rapidjson
6. zlib
