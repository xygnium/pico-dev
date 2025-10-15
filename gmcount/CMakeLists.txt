cmake_minimum_required(VERSION 3.13)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
include(pico_sdk_import.cmake)
project(gmcount_project C CXX ASM)

pico_sdk_init()

add_subdirectory(/home/mike/dev/pico/no-OS-FatFS-SD-SPI-RPi-Pico/FatFs_SPI build)
add_executable(gmcount
    gmcount.c
    hw_config.c
    fatfs.c
)

# Add the standard library and FatFS/SPI to the build
target_link_libraries(gmcount
    pico_stdlib
    FatFs_SPI
)

pico_set_program_name(gmcount "gmcount")
pico_set_program_version(gmcount "0.2")

#pico_enable_stdio_usb(gmcount 1)
pico_enable_stdio_uart(gmcount 1)
pico_add_extra_outputs(gmcount)
