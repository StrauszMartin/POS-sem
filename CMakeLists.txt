cmake_minimum_required(VERSION 3.16)
project(POS_sem C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(server src/server.c)
add_executable(client src/client.c)

target_include_directories(server PRIVATE src)
target_include_directories(client PRIVATE src)

target_compile_options(server PRIVATE -Wall -Wextra -g -O0)
target_compile_options(client PRIVATE -Wall -Wextra -g -O0)

target_link_libraries(server PRIVATE pthread)
target_link_libraries(client PRIVATE pthread)
