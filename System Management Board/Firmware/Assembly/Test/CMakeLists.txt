cmake_minimum_required(VERSION 3.13)

include(../../../../../pico-sdk/pico_sdk_init.cmake)

project(test C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

include_directories(${CMAKE_SOURCE_DIR})

add_executable(test
  Test.s
)

pico_add_extra_outputs(test)
target_link_libraries(test pico_stdlib)