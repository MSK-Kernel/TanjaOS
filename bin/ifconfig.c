#include <stdint.h>
#include <stddef.h>
#include "tanja.h"

static void print_ip(uint32_t ip) {
    printf("%d.%d.%d.%d", (int)((ip >> 24) & 255), (int)((ip >> 16) & 255),
           (int)((ip >> 8) & 255), (int)(ip & 255));
}

void main(char* args) {
    net_info_t ni;

    while (*args == ' ') args++;
    if (strcmp(args, "dhcp") == 0) {
        if (!net_get_info(&ni)) { print("ifconfig: no network adapter\n"); return; }
        print("requesting address...\n");
        if (net_dhcp() != 0) { print("ifconfig: DHCP failed (no server answered)\n"); return; }
    } else if (*args) {
        print("usage: ifconfig [dhcp]\n");
        return;
    }

    if (!net_get_info(&ni)) {
        print("no supported Ethernet adapter found\n");
        return;
    }
    printf("%s: link %s\n", ni.driver, ni.link ? "up" : "down (no cable?)");
    printf("  mac %02x:%02x:%02x:%02x:%02x:%02x\n",
           ni.mac[0], ni.mac[1], ni.mac[2], ni.mac[3], ni.mac[4], ni.mac[5]);
    if (!ni.configured) {
        print("  not configured (run: ifconfig dhcp)\n");
        return;
    }
    print("  ip      "); print_ip(ni.ip);   print("\n");
    print("  netmask "); print_ip(ni.mask); print("\n");
    print("  gateway "); print_ip(ni.gw);   print("\n");
    print("  dns     "); print_ip(ni.dns);  print("\n");
}
