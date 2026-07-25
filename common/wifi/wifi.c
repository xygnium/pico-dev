#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"

#include "wifi.h"

#define WIFI_UDP_BUF_SIZE 1024

static struct udp_pcb *s_pcb;
static wifi_udp_cmd_handler_t s_handler;
static volatile int s_cmd_pending;
static ip_addr_t s_recv_addr;
static u16_t s_recv_port;
static char s_cmd_buf[WIFI_UDP_BUF_SIZE];

#define WIFI_CONNECT_MAX_ATTEMPTS 5
#define WIFI_CONNECT_RETRY_DELAY_MS 1000

int wifi_connect(uint32_t country, const char *ssid, const char *pass, uint32_t auth)
{
    printf("wifi: connecting\n");
    if (cyw43_arch_init_with_country(country)) {
        return 1;
    }
    cyw43_arch_enable_sta_mode();

    int err = 0;
    for (int attempt = 1; attempt <= WIFI_CONNECT_MAX_ATTEMPTS; attempt++) {
        err = cyw43_arch_wifi_connect_blocking(ssid, pass, auth);
        if (err == 0) {
            break;
        }
        printf("wifi: connect attempt %d/%d failed (err %d)\n", attempt,
               WIFI_CONNECT_MAX_ATTEMPTS, err);
        sleep_ms(WIFI_CONNECT_RETRY_DELAY_MS);
    }
    if (err) {
        return 2;
    }
    printf("wifi: IP %s\n", ip4addr_ntoa(netif_ip_addr4(netif_default)));
    printf("wifi: Mask %s\n", ip4addr_ntoa(netif_ip_netmask4(netif_default)));
    printf("wifi: Gateway %s\n", ip4addr_ntoa(netif_ip_gw4(netif_default)));
    printf("wifi: Host name %s\n", netif_get_hostname(netif_default));
    return 0;
}

static void wifi_udp_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                              const ip_addr_t *addr, u16_t port)
{
    if (p == NULL) {
        return;
    }
    s_recv_addr = *addr;
    s_recv_port = port;
    size_t len = p->tot_len < sizeof(s_cmd_buf) - 1 ? p->tot_len : sizeof(s_cmd_buf) - 1;
    pbuf_copy_partial(p, s_cmd_buf, len, 0);
    s_cmd_buf[len] = 0;
    pbuf_free(p);
    s_cmd_pending = 1;
}

void wifi_udp_start(uint16_t port, wifi_udp_cmd_handler_t handler)
{
    s_handler = handler;
    s_pcb = udp_new();
    udp_recv(s_pcb, wifi_udp_recv_cb, NULL);
    udp_bind(s_pcb, IP_ADDR_ANY, port);
}

void wifi_udp_poll(void)
{
    if (!s_cmd_pending) {
        return;
    }
    char resp[WIFI_UDP_BUF_SIZE];
    resp[0] = 0;
    if (s_handler) {
        s_handler(s_cmd_buf, resp, sizeof(resp));
    }
    struct pbuf *q = pbuf_alloc(PBUF_TRANSPORT, strlen(resp) + 1, PBUF_RAM);
    if (q) {
        memcpy(q->payload, resp, strlen(resp) + 1);
        udp_sendto(s_pcb, q, &s_recv_addr, s_recv_port);
        pbuf_free(q);
    }
    s_cmd_pending = 0;
}
