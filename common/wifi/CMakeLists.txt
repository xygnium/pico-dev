add_library(picowifi wifi.c)

target_include_directories(picowifi PUBLIC ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(picowifi PUBLIC
    pico_stdlib
    pico_cyw43_arch_lwip_threadsafe_background
)
