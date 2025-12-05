cmake_minimum_required(VERSION 3.13)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
include(pico_sdk_import.cmake)
project(PicoW C CXX ASM)

#set(PICO_BOARD pico_w)
#set(PICO_BOARD pico2w)
#set(PICO_PLATFORM rp2350)
pico_sdk_init()

add_executable(main
 main.c
)

target_include_directories(main PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(main
    pico_stdlib
    #pico_cyw43_arch_lwip_threadsafe_background
)

pico_set_program_name(main "main")
pico_set_program_version(main "0.0")

pico_enable_stdio_usb(main 0)
pico_enable_stdio_uart(main 1)
pico_add_extra_outputs(main)
