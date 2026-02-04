#include <stdio.h>
#include "pico/stdlib.h"

//extern "C" void init_FatFs(void);
extern void init_FatFs(void);
extern void close_FatFs(void);
extern void listFiles(void);

extern int udp_setup(void);
extern int udp_recv_send(void);

int main() {
   stdio_init_all();
   udp_setup();
   int count = 1;

   //gpio_init(21);
   //gpio_set_function(21, GPIO_FUNC_SIO);
   //gpio_set_dir(21, GPIO_OUT);

   gpio_init(22);
   gpio_set_function(22, GPIO_FUNC_SIO);
   gpio_set_dir(22, GPIO_IN);
   //gpio_pull_up(22);

   printf("---- init_FatFs ---\n");
   init_FatFs();
   printf("---- list files ----\n");
   listFiles();
   printf("---- close_FatFs ---\n");
   close_FatFs();

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
        udp_recv_send();
        printf("hi\n");
        while (gpio_get(22) == 1) {};
    }
}

