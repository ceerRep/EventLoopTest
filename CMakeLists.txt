cmake_minimum_required(VERSION 3.16)

set(_project_name event-loop)

project(${_project_name} C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")

set(THREADS_PREFER_PTHREAD_FLAG ON)

find_package(Threads REQUIRED)
link_libraries(Threads::Threads)

find_package(Boost REQUIRED fiber)

find_package(fmt)
link_libraries(fmt::fmt)

add_subdirectory(function2)
link_libraries(function2)

set(MI_BUILD_TESTS OFF)
add_subdirectory(mimalloc)
link_libraries(mimalloc)

include_directories("${CMAKE_SOURCE_DIR}/include")
include_directories("${CMAKE_SOURCE_DIR}/concurrentqueue")

set (EVENTLOOP_SOURCE
    src/cpu.cpp
    src/stacktrace.cc
    src/mimalloc-override.cc
)

add_executable (event-loop-test 
    ${EVENTLOOP_SOURCE}
    src/main.cpp
)

add_executable (async-model-test 
    ${EVENTLOOP_SOURCE}
    test/src/main.cpp
    test/src/init.cpp
)

add_executable (direct-model-test 
    ${EVENTLOOP_SOURCE}
    test/src/main-direct.cpp
    test/src/init.cpp
)

add_executable (boost-fiber-test
    test/src/boost-fiber-test.cpp
    test/src/init.cpp
    # src/mimalloc-override.cc
)

target_include_directories(boost-fiber-test PUBLIC ${Boost_INCLUDE_DIR} )
target_link_libraries(boost-fiber-test ${Boost_LIBRARIES})

add_executable (std-thread-test
    test/src/std-thread-test.cpp
    test/src/init.cpp
    src/mimalloc-override.cc
    src/cpu.cpp
)

add_executable (std-thread-direct-test
    test/src/std-thread-direct-test.cpp
    test/src/init.cpp
    src/mimalloc-override.cc
    src/cpu.cpp
)

target_include_directories (async-model-test
    PUBLIC "${CMAKE_SOURCE_DIR}/test/include"
)

target_include_directories (direct-model-test 
    PUBLIC "${CMAKE_SOURCE_DIR}/test/include"
)

target_include_directories (boost-fiber-test
    PUBLIC "${CMAKE_SOURCE_DIR}/test/include"
)

target_include_directories (std-thread-test
    PUBLIC "${CMAKE_SOURCE_DIR}/test/include"
)

target_include_directories (std-thread-direct-test
    PUBLIC "${CMAKE_SOURCE_DIR}/test/include"
)
