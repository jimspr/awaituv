cmake_minimum_required (VERSION 2.6)
project (awaituv)

if (MSVC)
SET (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -await /std:c++lastest")
link_libraries (libuv libcurl)
endif ()

if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
SET (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -lpthread -fcoroutines-ts -std=c++14")
find_package (CURL REQUIRED)
link_libraries (${CURL_LIBRARIES})
link_libraries (libuv.a)
endif ()

include_directories (inc)
add_subdirectory (samples)
