cmake_minimum_required(VERSION 3.13)
include(pico_sdk_import.cmake)
project(gmcount_project C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()
add_executable(gmcount
gmcount.c
)
#pico_enable_stdio_usb(gmcount 1)
pico_enable_stdio_uart(gmcount 1)
pico_add_extra_outputs(gmcount)
target_link_libraries(gmcount pico_stdlib)
