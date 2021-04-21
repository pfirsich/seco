cmake_minimum_required(VERSION 3.0.0)

project(seco)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS on)

find_package(docopt COMPONENTS CXX REQUIRED)
find_package(Threads)
include_directories(${DOCOPT_INCLUDE_DIRS})

set(WARNING_FLAGS -Wall -Wextra -pedantic -Werror)

add_library(seco src/seco.cpp)
target_include_directories(seco PRIVATE include)
target_link_libraries (seco Threads::Threads)
target_compile_options(seco PRIVATE ${WARNING_FLAGS})

add_executable(example src/main.cpp)
target_include_directories(example PRIVATE include)
target_link_libraries (example Threads::Threads)
target_link_libraries(example docopt_s)
target_link_libraries(example seco)
target_compile_options(example PRIVATE ${WARNING_FLAGS})
