#include <stdio.h>
#include "pico/stdlib.h"
/*
#include "pico/cyw43_arch.h"

int setup(uint32_t country, const char *ssid, const char *pass,  uint32_t auth)
{
  printf("wifi setup\n");
  if (cyw43_arch_init_with_country(country))
  {
    return 1;
  }
  cyw43_arch_enable_sta_mode();
  if (cyw43_arch_wifi_connect_blocking(ssid, pass, auth))
  {
    return 2;
  }
}

char ssid[] = "abzu2";
char pass[] = "keyboardPlank20#";
uint32_t country = CYW43_COUNTRY_USA;
uint32_t auth = CYW43_AUTH_WPA2_MIXED_PSK;
*/

int main()
{
    stdio_init_all();
    //setup_default_uart();
    printf("init stdio complete\n");
    /*
    setup(country, ssid, pass, auth);
    char *ip4addr_sp = ip4addr_ntoa(netif_ip_addr4(netif_default));
    printf("IP: %s\n", ip4addr_sp);
    printf("Mask: %s\n", ip4addr_ntoa(netif_ip_netmask4(netif_default)));
    printf("Gateway: %s\n", ip4addr_ntoa(netif_ip_gw4(netif_default)));
    printf("Host Name: %s\n", netif_get_hostname(netif_default));    
    */
    printf("forever loop\n");
    while (1)
    {
        sleep_ms(2000);
        printf("Pico2 W is running!\n");
    }
}
