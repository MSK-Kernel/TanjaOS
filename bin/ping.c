#include <stdint.h>
#include <stddef.h>
#include "tanja.h"

static void print_ip(uint32_t ip) {
    printf("%d.%d.%d.%d", (int)((ip >> 24) & 255), (int)((ip >> 16) & 255),
           (int)((ip >> 8) & 255), (int)(ip & 255));
}

void main(char* args) {
    net_info_t ni;
    uint32_t ip;
    int i, got = 0, rtt;

    while (*args == ' ') args++;
    if (!*args) { print("usage: ping <host or ip>\n"); return; }
    if (!net_get_info(&ni) || !ni.configured) {
        print("ping: network not configured (try: ifconfig dhcp)\n");
        return;
    }
    if (net_resolve(args, &ip) != 0) {
        printf("ping: cannot resolve %s\n", args);
        return;
    }
    printf("PING %s (", args); print_ip(ip); print(")\n");
    for (i = 0; i < 4; i++) {
        rtt = net_ping(ip, 1500);
        if (rtt >= 0) {
            got++;
            print("reply from "); print_ip(ip);
            if (rtt == 0) printf(": seq=%d time<1 ms\n", i + 1);
            else printf(": seq=%d time=%d ms\n", i + 1, rtt);
        } else {
            printf("seq=%d: no reply\n", i + 1);
        }
        if (i < 3) timer_delay_ms(1000);
    }
    printf("%d sent, %d received\n", 4, got);
}
