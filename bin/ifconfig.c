#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include "tanja.h"

/* ifconfig                      show the active interface
 * ifconfig list                 every Ethernet card found, with or without a driver
 * ifconfig use <N>              make card N (see list) the active interface, then run DHCP
 * ifconfig dhcp                 ask a DHCP server again (and forget any saved static setup)
 * ifconfig static <ip>[/bits] [gw <ip>] [dns <ip>]   fixed address, saved in /etc/net.conf (root) */

static void print_ip(uint32_t ip) {
    printf("%d.%d.%d.%d", (int)((ip >> 24) & 255), (int)((ip >> 16) & 255),
           (int)((ip >> 8) & 255), (int)(ip & 255));
}

static int parse_ip(const char* s, uint32_t* ip, int* prefix) {
    uint32_t v = 0; int parts = 0, cur = -1;
    if (prefix) *prefix = -1;
    for (;; s++) {
        if (*s >= '0' && *s <= '9') { cur = (cur < 0 ? 0 : cur) * 10 + (*s - '0'); if (cur > 255) return 0; }
        else if (*s == '.' || *s == 0 || *s == '/') {
            if (cur < 0) return 0;
            v = (v << 8) | cur; parts++; cur = -1;
            if (*s == 0) break;
            if (*s == '/') {
                int p = 0, any = 0;
                s++;
                while (*s >= '0' && *s <= '9') { p = p * 10 + (*s - '0'); any = 1; s++; }
                if (!any || *s || p > 32 || !prefix) return 0;
                *prefix = p;
                break;
            }
        } else return 0;
    }
    if (parts != 4) return 0;
    *ip = v;
    return 1;
}

static const char* word(char** pp, char* out, int cap) {
    char* p = *pp; int n = 0;
    while (*p == ' ') p++;
    while (*p && *p != ' ' && n < cap - 1) out[n++] = *p++;
    out[n] = 0; *pp = p;
    return n ? out : 0;
}

static void show_nic(net_nic_info_t* n, int i) {
    printf("%c eth%d  ", n->active ? '*' : ' ', i);
    if (n->state == NIC_ST_READY)
        printf("%-10s %04x:%04x  %02x:%02x:%02x:%02x:%02x:%02x  link %s\n", n->name, n->vendor, n->device,
               n->mac[0], n->mac[1], n->mac[2], n->mac[3], n->mac[4], n->mac[5], n->link ? "up" : "down");
    else if (n->state == NIC_ST_NODRIVER)
        printf("%04x:%04x at pci %02x:%02x.%d  - no driver for this card yet\n", n->vendor, n->device, n->bus, n->dev, n->fn);
    else if (n->state == NIC_ST_FAILED)
        printf("%04x:%04x (%s)  - driver could not start the card\n", n->vendor, n->device, n->name);
    else
        printf("%04x:%04x (%s)  - second card of this kind, not used\n", n->vendor, n->device, n->name);
}

static int cmd_list(void) {
    net_nic_info_t n;
    int i, c = net_nic_count();
    if (!c) { print("no Ethernet controller found on the PCI bus\n"); return 0; }
    for (i = 0; i < c; i++) { net_nic_info(i, &n); show_nic(&n, i); }
    print("(* = in use)\n");
    return 0;
}

static void show_active(void) {
    net_info_t ni;
    net_nic_info_t n;
    int i, c = net_nic_count(), others = 0;
    if (!net_get_info(&ni)) {
        print("no usable Ethernet adapter\n");
        if (c) { print("cards found:\n"); cmd_list(); }
        else print("(no Ethernet controller on the PCI bus at all)\n");
        return;
    }
    printf("%s: link %s\n", ni.driver, ni.link ? "up" : "down (no cable?)");
    printf("  mac %02x:%02x:%02x:%02x:%02x:%02x\n", ni.mac[0], ni.mac[1], ni.mac[2], ni.mac[3], ni.mac[4], ni.mac[5]);
    if (!ni.configured) print("  not configured (run: ifconfig dhcp)\n");
    else {
        print("  ip      "); print_ip(ni.ip);   print("\n");
        print("  netmask "); print_ip(ni.mask); print("\n");
        print("  gateway "); print_ip(ni.gw);   print("\n");
        print("  dns     "); print_ip(ni.dns);  print("\n");
    }
    printf("  frames  rx %u  tx %u  tx errors %u\n", ni.rx_frames, ni.tx_frames, ni.tx_errors);
    for (i = 0; i < c; i++) { net_nic_info(i, &n); if (!n.active) others++; }
    if (others) printf("  (%d other card%s: ifconfig list)\n", others, others == 1 ? "" : "s");
}

static int do_dhcp(void) {
    net_info_t ni;
    if (!net_get_info(&ni)) { print("ifconfig: no usable adapter\n"); return 1; }
    print("requesting address...\n");
    if (net_dhcp() != 0) {
        print("ifconfig: DHCP failed (no server answered)\n");
        if (!ni.link) print("          the link is down - is a cable plugged in?\n");
        else if (!ni.rx_frames) print("          this card has received no frames at all: wrong port, cable, or a driver problem\n");
        return 1;
    }
    return 0;
}

void main(char* args) {
    char w[40], a[40];
    char* p = args;

    if (!word(&p, w, sizeof(w))) { show_active(); return; }

    if (strcmp(w, "list") == 0) { cmd_list(); return; }

    if (strcmp(w, "dhcp") == 0) {
        if (fs_file_exists("/etc/net.conf") && fs_cur_uid == 0) fs_delete_file("/etc/net.conf");
        if (do_dhcp() == 0) show_active();
        return;
    }

    if (strcmp(w, "use") == 0) {
        int idx;
        if (!word(&p, a, sizeof(a)) || !isdigit(a[0])) { print("usage: ifconfig use <N>   (N from: ifconfig list)\n"); return; }
        idx = atoi(a);
        if (net_nic_use(idx) != 0) { printf("ifconfig: eth%d is not a usable card\n", idx); return; }
        if (do_dhcp() == 0) show_active();
        return;
    }

    if (strcmp(w, "static") == 0) {
        uint32_t ip = 0, mask, gw = 0, dns = 0, t;
        int prefix, have_gw = 0, have_dns = 0;
        char buf[200];
        net_info_t ni;
        if (!word(&p, a, sizeof(a)) || !parse_ip(a, &ip, &prefix)) { print("usage: ifconfig static <ip>[/bits] [gw <ip>] [dns <ip>]\n"); return; }
        if (prefix < 0) prefix = 24;
        mask = prefix ? (0xFFFFFFFFu << (32 - prefix)) : 0;
        while (word(&p, w, sizeof(w))) {
            if (!word(&p, a, sizeof(a)) || !parse_ip(a, &t, 0)) { print("ifconfig: bad address after the keyword\n"); return; }
            if (strcmp(w, "gw") == 0) { gw = t; have_gw = 1; }
            else if (strcmp(w, "dns") == 0) { dns = t; have_dns = 1; }
            else { printf("ifconfig: unknown option '%s'\n", w); return; }
        }
        if (!have_dns) dns = gw;
        if (!net_get_info(&ni)) { print("ifconfig: no usable adapter\n"); return; }
        net_set_static(ip, mask, gw, dns);
        if (fs_cur_uid == 0) {
            if (!fs_directory_exists("/etc")) fs_create_directory("/etc");
            snprintf(buf, sizeof(buf), "ip %d.%d.%d.%d\nmask %d.%d.%d.%d\ngw %d.%d.%d.%d\ndns %d.%d.%d.%d\n",
                     (int)(ip >> 24), (int)((ip >> 16) & 255), (int)((ip >> 8) & 255), (int)(ip & 255),
                     (int)(mask >> 24), (int)((mask >> 16) & 255), (int)((mask >> 8) & 255), (int)(mask & 255),
                     (int)(gw >> 24), (int)((gw >> 16) & 255), (int)((gw >> 8) & 255), (int)(gw & 255),
                     (int)(dns >> 24), (int)((dns >> 16) & 255), (int)((dns >> 8) & 255), (int)(dns & 255));
            if (fs_write_file("/etc/net.conf", buf, strlen(buf)) == 0) print("saved to /etc/net.conf (used at every boot)\n");
        } else print("applied for this session only (run as root to save it)\n");
        (void)have_gw;
        show_active();
        return;
    }

    print("usage: ifconfig [list | use <N> | dhcp | static <ip>[/bits] [gw <ip>] [dns <ip>]]\n");
}
