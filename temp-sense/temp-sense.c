#include <stdio.h>
#include "pico/stdlib.h"

extern int example_ds18b20();

int main() {
    stdio_init_all();
    printf("Hello, world!\n");
    example_ds18b20();
    for (;;);
}
