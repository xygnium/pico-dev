#include <stdio.h>
#include "pico/stdlib.h"

extern int wifi_loop(void);

int main()
{
    stdio_init_all();
    printf("init stdio complete\n");
    wifi_loop();
}
