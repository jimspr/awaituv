cmake_minimum_required (VERSION 2.6)
project (awaituv)


if (MSVC)
SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -await")
link_libraries (libuv)
endif()

if(UNIX)
SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -lpthread -fcoroutines-ts")
link_libraries (libuv.a)
endif()

set (CMAKE_CXX_STANDARD 14)

include_directories(inc)
add_subdirectory(samples)
