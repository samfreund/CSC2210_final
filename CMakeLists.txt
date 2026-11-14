cmake_minimum_required(VERSION 3.16)
project(srtpsender CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

# Vendored libdatachannel: static build, no examples/tests.
set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/libdatachannel)

add_executable(sender src/sender.cpp)
target_link_libraries(sender PRIVATE datachannel-static Threads::Threads)
