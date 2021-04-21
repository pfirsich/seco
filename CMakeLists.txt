cmake_minimum_required(VERSION 3.0.0)

project(server-control)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS on)

include(cmake/wall.cmake)

if (SERVER_CONTROL_ENABLE_ASAN)
  include(cmake/asan.cmake)
endif()

find_package(docopt COMPONENTS CXX REQUIRED)
find_package(Threads)
include_directories(${DOCOPT_INCLUDE_DIRS})

set(SRC
    main.cpp
    servercontrol.cpp
)
list(TRANSFORM SRC PREPEND src/)

add_executable(server-control ${SRC})
target_include_directories(server-control PUBLIC include)
target_link_libraries(server-control docopt_s)
target_link_libraries (server-control Threads::Threads)
set_wall(server-control)
