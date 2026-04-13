#include <stdio.h>
#include "pico/stdlib.h"

extern int example_ds3231();

int main() {
    stdio_init_all();
    printf("Hello, world!\n");
    example_ds3231();
    for (;;);
}
