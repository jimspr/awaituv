cmake_minimum_required (VERSION 2.6)
project (awaituv)

if (MSVC)
SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -await")
link_libraries (libuv libcurl)
endif()

if(UNIX)
SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -lpthread -fcoroutines-ts")
find_package(CURL REQUIRED)
link_libraries(${CURL_LIBRARIES})
link_libraries (libuv.a)
endif()

set (CMAKE_CXX_STANDARD 14)

include_directories(inc)
add_subdirectory(samples)
