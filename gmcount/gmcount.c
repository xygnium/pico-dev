#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"

#include "api_ds3231.h"

// Modify these definitions as required, to match connections.
#define DS3231_I2C_PORT i2c1
#define DS3231_I2C_SDA_PIN 26
#define DS3231_I2C_SCL_PIN 27

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

    // Create a real-time clock structure and initiate this.
    struct ds3231_rtc rtc;
    ds3231_init(DS3231_I2C_PORT, DS3231_I2C_SDA_PIN,
                DS3231_I2C_SCL_PIN, &rtc);
    
    // A `ds3231_datetime_t` structure holds date and time values. It is
    // used first to set the initial (user-defined) date/time. After
    // these initial values are set, then this same structure will be
    // updated by the ds3231 functions as needed with the most current
    // time and date values.
    ds3231_datetime_t dt;

    // This is a character array that will be used for storing a string
    // representation of the date and time.
    uint8_t dt_str[25];

    while (true) {
        printf("lo\n");
        while (gpio_get(22) == 0) {};
        // transition from lo to hi detected
        printf("count=%d\n", count);
        count++;
        // Read the date and time from the DS3231 RTC.
        ds3231_get_datetime(&dt, &rtc);
        // Convert the dt structure to a string and print this.
        ds3231_ctime(dt_str, sizeof(dt_str), &dt);
        puts(dt_str);
        udp_recv_send();
        printf("hi\n");
        while (gpio_get(22) == 1) {};
    }
}

