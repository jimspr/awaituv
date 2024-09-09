cmake_minimum_required (VERSION 3.1)
project (awaituv)


if ("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
MESSAGE("Clang doesn't support C++20 coroutines")
#SET (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -lpthread -fcoroutines-ts -std=c++14")
#find_package (CURL REQUIRED)
#link_libraries (${CURL_LIBRARIES})
#link_libraries (libuv.a)
elseif ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
SET (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -lpthread -fcoroutines -std=c++20")
find_package (CURL REQUIRED)
link_libraries (${CURL_LIBRARIES} uv)
elseif ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "MSVC")
SET (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /std:c++latest")
link_libraries (libcurl uv)

endif ()

include_directories (inc)
add_subdirectory (samples)
