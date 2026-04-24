#include <stdio.h>

#include "pico/stdlib.h"
#include "hardware/i2c.h"

#include "api_ds3231.h"

// Modify these definitions as required, to match connections.
#define DS3231_I2C_PORT i2c1
#define DS3231_I2C_SDA_PIN 26
#define DS3231_I2C_SCL_PIN 27

// --- external function declaration ---

extern void init_FatFs(void);
extern void close_FatFs(void);
extern void listFiles(void);

extern int udp_setup(void);
extern int udp_recv_send(void);

// --- global variables ---

volatile int count = 1;

// --------------------------------------------
// Callback function triggered by the interrupt
void gpio_callback(uint gpio, uint32_t events) {
    if (events & GPIO_IRQ_EDGE_RISE) {
        //printf("ISR:GPIO %d went High!\n", gpio);
	count++;
    }
}

// --------------------------------------------
int main() {
   stdio_init_all();
   udp_setup();
   const uint INPUT_PIN = 22;

   gpio_init(INPUT_PIN);
   // gpio_set_function() call is redundant; called by gpio_init()
   gpio_set_function(INPUT_PIN, GPIO_FUNC_SIO);
   gpio_set_dir(INPUT_PIN, GPIO_IN);
   //gpio_pull_up(INPUT_PIN);

    // Set up the interrupt for a rising edge (low to high)
    gpio_set_irq_enabled_with_callback(
		    INPUT_PIN,
		    GPIO_IRQ_EDGE_RISE,
		    true,
		    &gpio_callback);

   printf("---- init_FatFs ---\n");
   init_FatFs();
   printf("---- list files ----\n");
   listFiles();
   printf("---- close_FatFs ---\n");
   close_FatFs();

    /*
    while (1) {
        // Read the state of GPIO pin INPUT_PIN
        int pin_state = gpio_get(INPUT_PIN);

        if (pin_state == 1) {
            // Pin INPUT_PIN is HIGH
            printf("Pin INPUT_PIN is HIGH\n");
        } else {
            // Pin INPUT_PIN is LOW
            printf("Pin INPUT_PIN is LOW\n");
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

    /*
    while (true) {
        printf("lo\n");
        while (gpio_get(INPUT_PIN) == 0) {};
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
        while (gpio_get(INPUT_PIN) == 1) {};
    }
    */

    while (true) {
        // Read the date and time from the DS3231 RTC.
        ds3231_get_datetime(&dt, &rtc);
        // Convert the dt structure to a string and print this.
        ds3231_ctime(dt_str, sizeof(dt_str), &dt);
        puts(dt_str);
        udp_recv_send();
        printf("count=%d\n", count);
        sleep_ms(5000);
    }
}

