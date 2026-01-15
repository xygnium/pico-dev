#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"

int setup(uint32_t country, const char *ssid, const char *pass,  uint32_t auth)
{
  printf("wifi setup\n");
  if (cyw43_arch_init_with_country(country))
  {
    // fail
    return 1;
  }
  cyw43_arch_enable_sta_mode();
  if (cyw43_arch_wifi_connect_blocking(ssid, pass, auth))
  {
    // fail
    return 2;
  }
  char *ip4addr_sp = ip4addr_ntoa(netif_ip_addr4(netif_default));
  printf("IP: %s\n", ip4addr_sp);
  printf("Mask: %s\n", ip4addr_ntoa(netif_ip_netmask4(netif_default)));
  printf("Gateway: %s\n", ip4addr_ntoa(netif_ip_gw4(netif_default)));
  printf("Host Name: %s\n", netif_get_hostname(netif_default));    
  return 0;
}

#define BUF_SIZE 1024

void recv(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
    char myBuff[BUF_SIZE];
    if (p != NULL)
    {
        printf("recv total %d  this buffer %d next %d \n", p->tot_len, p->len, p->next);
        printf("From %s:%d\n", ipaddr_ntoa(addr), port);
        pbuf_copy_partial(p, myBuff, p->tot_len, 0);
        myBuff[p->tot_len] = 0;
        printf("Buffer= %s\n", myBuff);
        pbuf_free(p);
    }
}

char ssid[] = "abzu2";
char pass[] = "keyboardPlank20#";
uint32_t country = CYW43_COUNTRY_USA;
uint32_t auth = CYW43_AUTH_WPA2_MIXED_PSK;

int main()
{
    stdio_init_all();
    printf("init stdio complete\n");
    setup(country, ssid, pass, auth);
    printf("forever loop\n");
    struct udp_pcb *pcb = udp_new();
    udp_recv(pcb, recv, NULL);
    udp_bind(pcb, IP_ADDR_ANY, 8080);
    while (true)
    {
        sleep_ms(1);
    }
}
