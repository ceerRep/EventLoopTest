cmake_minimum_required(VERSION 3.16)

set(_project_name async-layer-test)

project(${_project_name} C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

set(THREADS_PREFER_PTHREAD_FLAG ON)

find_package(Threads REQUIRED)
link_libraries(Threads::Threads)

find_package(fmt)
link_libraries(fmt::fmt)

add_subdirectory(function2)
# function2 provides an interface target which makes it's
# headers available to all projects using function2


include_directories("${CMAKE_SOURCE_DIR}/include")

add_executable (async-layer-test 
    src/cpu.cpp
    src/main.cpp
)

target_link_libraries(async-layer-test function2)
