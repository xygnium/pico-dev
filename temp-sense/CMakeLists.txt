cmake_minimum_required(VERSION 3.13)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
include(pico_sdk_import.cmake)
project(temp_sense_project C CXX ASM)

pico_sdk_init()

add_subdirectory(onewire_library)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../common/wifi common_wifi)

add_executable(temp-sense
    temp-sense.c
    use_ds18b20.c
    api_ds3231.c
)

target_include_directories(temp-sense PRIVATE ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(temp-sense
    pico_stdlib
    hardware_i2c
    hardware_pio
    onewire_library
    picowifi
)

pico_set_program_name(temp-sense "temp-sense")
pico_set_program_version(temp-sense "0.1")

pico_enable_stdio_usb(temp-sense 0)
pico_enable_stdio_uart(temp-sense 1)
pico_add_extra_outputs(temp-sense)
