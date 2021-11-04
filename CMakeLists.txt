cmake_minimum_required(VERSION 3.16)

set(_project_name event-loop)

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
link_libraries(function2)

include_directories("${CMAKE_SOURCE_DIR}/include")

add_executable (event-loop-test 
    src/cpu.cpp
    src/main.cpp
)

add_executable (async-model-test 
    test/src/main.cpp
)

target_include_directories (async-model-test
    PUBLIC "${CMAKE_SOURCE_DIR}/test/include"
)
