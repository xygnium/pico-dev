// Function commands for d218b20 1-Wire temperature sensor
// https://www.analog.com/en/products/ds18b20.html
//
#define DS18B20_FAMILY_CODE         0x28  // low byte of every DS18B20 ROM code
#define DS18B20_CONVERT_T           0x44
#define DS18B20_WRITE_SCRATCHPAD    0x4e
#define DS18B20_READ_SCRATCHPAD     0xbe
#define DS18B20_COPY_SCRATCHPAD     0x48
#define DS18B20_RECALL_EE           0xb8
#define DS18B20_READ_POWER_SUPPLY   0xb4