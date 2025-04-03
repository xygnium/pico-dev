#include <stdio.h>
#include "pico/stdlib.h"

int main() {
   stdio_init_all();
   int count = 1;

   //gpio_init(21);
   //gpio_set_function(21, GPIO_FUNC_SIO);
   //gpio_set_dir(21, GPIO_OUT);

   gpio_init(22);
   gpio_set_function(22, GPIO_FUNC_SIO);
   gpio_set_dir(22, GPIO_IN);
   //gpio_pull_up(22);

   printf("---- Hello, world ----\n");

    /*
    while (1) {
        // Read the state of GPIO pin 22
        int pin_state = gpio_get(22);

        if (pin_state == 1) {
            // Pin 22 is HIGH
            printf("Pin 22 is HIGH\n");
        } else {
            // Pin 22 is LOW
            printf("Pin 22 is LOW\n");
        }
        // Add a delay (optional)
        sleep_ms(1000);
    }
    */

    while (true) {
        printf("lo\n");
        while (gpio_get(22) == 0) {};
        // transition from lo to hi detected
        printf("count=%d\n", count);
        count++;
        printf("hi\n");
        while (gpio_get(22) == 1) {};
    }
}

