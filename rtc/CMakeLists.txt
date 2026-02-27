cmake_minimum_required(VERSION 3.13)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
include(pico_sdk_import.cmake)
project(rtc_project C CXX ASM)

pico_sdk_init()

add_executable(rtc
    rtc.c
    use_ds3231.c
    api_ds3231.c
)

target_include_directories(rtc PRIVATE ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(rtc
    pico_stdlib
    hardware_i2c
)

pico_set_program_name(rtc "rtc")
pico_set_program_version(rtc "0.1")

pico_enable_stdio_usb(rtc 0)
pico_enable_stdio_uart(rtc 1)
pico_add_extra_outputs(rtc)
