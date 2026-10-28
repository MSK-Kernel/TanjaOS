/* net.c - TanjaOS network stack (IPv4 over Ethernet, polled).
 *
 * ARP, ICMP echo, UDP, DHCP client, DNS resolver, a single-connection
 * TCP client and a plain-HTTP GET on top of it. There is no TLS, so no
 * https. Everything is driven by net_poll(), which the keyboard idle
 * loop and every blocking call here pump.
 */
#include <stdint.h>
#include <stddef.h>
#include "../include/net.h"

extern void  print(const char* s);
extern int   printf(const char* fmt, ...);
extern void  boot_log(const char* msg);
extern void  timer_delay_ms(uint32_t ms);
extern uint32_t get_uptime_ms(void);
extern void* memset(void* d, int v, unsigned int n);
extern void* memcpy(void* d, const void* s, unsigned int n);
extern void* memmove(void* d, const void* s, unsigned int n);
extern int   memcmp(const void* a, const void* b, unsigned int n);
extern int   strlen(const char* s);
extern int   strncasecmp(const char* a, const char* b, unsigned int n);
extern char* strcpy(char* d, const char* s);
extern char* strncpy(char* d, const char* s, unsigned int n);

extern int  tls_start(const char* host, int (*rd)(unsigned char*, size_t), int (*wr)(const unsigned char*, size_t));
extern int  tls_write_all(const void* b, size_t n);
extern int  tls_read(void* b, size_t n);
extern void tls_end(void);
extern const char* tls_error_str(void);

/* ------------------------------------------------------------ helpers */

static inline uint16_t rd16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint32_t rd32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline void wr16(uint8_t* p, uint16_t v) { p[0] = v >> 8; p[1] = (uint8_t)v; }
static inline void wr32(uint8_t* p, uint32_t v) {
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (uint8_t)v;
}

static uint32_t csum_add(uint32_t sum, const uint8_t* p, uint32_t len) {
    while (len > 1) { sum += (p[0] << 8) | p[1]; p += 2; len -= 2; }
    if (len) sum += p[0] << 8;
    return sum;
}
static uint16_t csum_fin(uint32_t sum) {
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

static void ip_str(uint32_t ip, char* o) {
    int i, n = 0;
    for (i = 3; i >= 0; i--) {
        uint32_t b = (ip >> (i * 8)) & 0xFF;
        if (b >= 100) o[n++] = '0' + b / 100;
        if (b >= 10)  o[n++] = '0' + (b / 10) % 10;
        o[n++] = '0' + b % 10;
        if (i) o[n++] = '.';
    }
    o[n] = 0;
}

/* -------------------------------------------------------------- state */

static nic_t nic;
static int   nic_ok;
static struct { uint32_t ip, mask, gw, dns; } cfg;
static uint8_t BCAST[6] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };

static uint8_t rxf[2048];
static uint8_t txf[1600];

/* ---------------------------------------------------------------- ARP */

static struct { uint32_t ip; uint8_t mac[6]; int ok; } arp_tab[8];
static int arp_next;

static void arp_learn(uint32_t ip, const uint8_t* mac) {
    int i;
    for (i = 0; i < 8; i++)
        if (arp_tab[i].ok && arp_tab[i].ip == ip) { memcpy(arp_tab[i].mac, mac, 6); return; }
    arp_tab[arp_next].ip = ip;
    memcpy(arp_tab[arp_next].mac, mac, 6);
    arp_tab[arp_next].ok = 1;
    arp_next = (arp_next + 1) % 8;
}

static int arp_lookup(uint32_t ip, uint8_t* mac) {
    int i;
    for (i = 0; i < 8; i++)
        if (arp_tab[i].ok && arp_tab[i].ip == ip) { memcpy(mac, arp_tab[i].mac, 6); return 1; }
    return 0;
}

static int eth_send(const uint8_t* dst, uint16_t type, const uint8_t* payload, uint32_t len) {
    if (!nic_ok || len > 1500) return -1;
    memcpy(txf, dst, 6);
    memcpy(txf + 6, nic.mac, 6);
    wr16(txf + 12, type);
    memcpy(txf + 14, payload, len);
    return nic.send(txf, 14 + len);
}

static void arp_send(uint16_t op, const uint8_t* tmac, uint32_t tip) {
    uint8_t p[28];
    wr16(p, 1); wr16(p + 2, 0x0800); p[4] = 6; p[5] = 4; wr16(p + 6, op);
    memcpy(p + 8, nic.mac, 6);
    wr32(p + 14, cfg.ip);
    if (tmac) memcpy(p + 18, tmac, 6); else memset(p + 18, 0, 6);
    wr32(p + 24, tip);
    eth_send(op == 1 ? BCAST : tmac, 0x0806, p, 28);
}

static void arp_in(const uint8_t* p, uint32_t len) {
    if (len < 28 || rd16(p) != 1 || rd16(p + 2) != 0x0800) return;
    uint16_t op = rd16(p + 6);
    uint32_t sip = rd32(p + 14), tip = rd32(p + 24);
    if (sip) arp_learn(sip, p + 8);
    if (op == 1 && cfg.ip && tip == cfg.ip) arp_send(2, p + 8, sip);
}

static void net_poll_inner(void);

static int arp_resolve(uint32_t ip, uint8_t* mac) {
    int tries;
    if (ip == 0xFFFFFFFFu) { memcpy(mac, BCAST, 6); return 0; }
    uint32_t hop = ((ip ^ cfg.ip) & cfg.mask) == 0 ? ip : cfg.gw;
    if (!hop) return -1;
    if (arp_lookup(hop, mac)) return 0;
    for (tries = 0; tries < 4; tries++) {
        arp_send(1, 0, hop);
        uint32_t t0 = get_uptime_ms();
        while (get_uptime_ms() - t0 < 300) {
            net_poll_inner();
            if (arp_lookup(hop, mac)) return 0;
            asm volatile ("hlt");
        }
    }
    return -1;
}

/* --------------------------------------------------------------- IPv4 */

static uint16_t ip_id;

static int ip_send(uint8_t proto, uint32_t dst, const uint8_t* data, uint32_t len) {
    static uint8_t pkt[1500];
    uint8_t mac[6];
    if (len > 1480) return -1;
    if (arp_resolve(dst, mac) != 0) return -1;
    pkt[0] = 0x45; pkt[1] = 0;
    wr16(pkt + 2, 20 + len);
    wr16(pkt + 4, ++ip_id);
    wr16(pkt + 6, 0x4000);               /* DF */
    pkt[8] = 64; pkt[9] = proto;
    wr16(pkt + 10, 0);
    wr32(pkt + 12, cfg.ip);
    wr32(pkt + 16, dst);
    wr16(pkt + 10, csum_fin(csum_add(0, pkt, 20)));
    memcpy(pkt + 20, data, len);
    return eth_send(mac, 0x0800, pkt, 20 + len);
}

/* --------------------------------------------------------------- ICMP */

static volatile int ping_got;
static uint16_t ping_seq;
#define PING_ID 0x544A

static void icmp_in(uint32_t src, const uint8_t* p, uint32_t len) {
    if (len < 8) return;
    if (p[0] == 8 && cfg.ip) {                       /* echo request */
        static uint8_t r[1480];
        if (len > sizeof(r)) return;
        memcpy(r, p, len);
        r[0] = 0; wr16(r + 2, 0);
        wr16(r + 2, csum_fin(csum_add(0, r, len)));
        ip_send(1, src, r, len);
    } else if (p[0] == 0) {                          /* echo reply */
        if (rd16(p + 4) == PING_ID && rd16(p + 6) == ping_seq) ping_got = 1;
    }
}

/* ---------------------------------------------------------------- UDP */

static struct {
    uint16_t port; volatile int ready;
    uint32_t src; uint16_t sport;
    int len; uint8_t buf[600];
} us;

static int udp_send(uint32_t dst, uint16_t sport, uint16_t dport, const uint8_t* data, uint32_t len) {
    static uint8_t d[1480];
    if (len > 1472) return -1;
    wr16(d, sport); wr16(d + 2, dport); wr16(d + 4, 8 + len); wr16(d + 6, 0);
    memcpy(d + 8, data, len);
    return ip_send(17, dst, d, 8 + len);
}

static void udp_in(uint32_t src, const uint8_t* p, uint32_t len) {
    if (len < 8 || us.ready) return;
    if (rd16(p + 2) != us.port || !us.port) return;
    uint32_t n = rd16(p + 4);
    if (n < 8 || n > len) return;
    n -= 8;
    if (n > sizeof(us.buf)) n = sizeof(us.buf);
    memcpy(us.buf, p + 8, n);
    us.len = n; us.src = src; us.sport = rd16(p);
    us.ready = 1;
}

/* ---------------------------------------------------------------- TCP */

#define T_FIN 0x01
#define T_SYN 0x02
#define T_RST 0x04
#define T_PSH 0x08
#define T_ACK 0x10

enum { TS_CLOSED, TS_SYN_SENT, TS_ESTAB, TS_CLOSE_WAIT };
#define T_RING 16384

static struct {
    int state;
    uint32_t rip; uint16_t rport, lport;
    uint32_t snd_una, snd_nxt, rcv_nxt;
    uint16_t mss;
    volatile int fin, rst;
    uint8_t ring[T_RING];
    uint32_t rhead, rtail, rcount;
} tc;

static int tcp_tx(uint32_t seq, uint8_t flags, const uint8_t* data, uint32_t len, int mss_opt) {
    static uint8_t seg[1480];
    uint32_t hl = mss_opt ? 24 : 20;
    if (hl + len > sizeof(seg)) return -1;
    wr16(seg, tc.lport); wr16(seg + 2, tc.rport);
    wr32(seg + 4, seq);
    wr32(seg + 8, (flags & T_ACK) ? tc.rcv_nxt : 0);
    seg[12] = (hl / 4) << 4; seg[13] = flags;
    wr16(seg + 14, T_RING - tc.rcount);
    wr16(seg + 16, 0); wr16(seg + 18, 0);
    if (mss_opt) { seg[20] = 2; seg[21] = 4; wr16(seg + 22, 1460); }
    if (len) memcpy(seg + hl, data, len);
    uint8_t ph[12];
    wr32(ph, cfg.ip); wr32(ph + 4, tc.rip); ph[8] = 0; ph[9] = 6; wr16(ph + 10, hl + len);
    uint32_t s = csum_add(0, ph, 12);
    s = csum_add(s, seg, hl + len);
    wr16(seg + 16, csum_fin(s));
    return ip_send(6, tc.rip, seg, hl + len);
}

static void tcp_in(uint32_t src, const uint8_t* p, uint32_t len) {
    if (len < 20 || tc.state == TS_CLOSED) return;
    if (src != tc.rip || rd16(p) != tc.rport || rd16(p + 2) != tc.lport) return;
    uint32_t seq = rd32(p + 4), ack = rd32(p + 8);
    uint32_t hl = (p[12] >> 4) * 4;
    uint8_t fl = p[13];
    if (hl < 20 || hl > len) return;
    const uint8_t* d = p + hl;
    uint32_t dlen = len - hl;

    if (fl & T_RST) { tc.rst = 1; tc.state = TS_CLOSED; return; }

    if (tc.state == TS_SYN_SENT) {
        if ((fl & (T_SYN | T_ACK)) == (T_SYN | T_ACK) && ack == tc.snd_nxt) {
            tc.rcv_nxt = seq + 1;
            tc.snd_una = ack;
            tc.mss = 536;
            uint32_t o = 20;
            while (o + 1 < hl) {                        /* options */
                if (p[o] == 0) break;
                if (p[o] == 1) { o++; continue; }
                if (p[o + 1] < 2 || o + p[o + 1] > hl) break;
                if (p[o] == 2 && p[o + 1] == 4) tc.mss = rd16(p + o + 2);
                o += p[o + 1];
            }
            if (tc.mss > 1460) tc.mss = 1460;
            tc.state = TS_ESTAB;
            tcp_tx(tc.snd_nxt, T_ACK, 0, 0, 0);
        }
        return;
    }

    if ((fl & T_ACK) && (int32_t)(ack - tc.snd_una) > 0 && (int32_t)(ack - tc.snd_nxt) <= 0)
        tc.snd_una = ack;

    int need_ack = 0;
    if (dlen) {
        if (seq == tc.rcv_nxt) {
            uint32_t room = T_RING - tc.rcount, n = dlen < room ? dlen : room, i;
            for (i = 0; i < n; i++) { tc.ring[tc.rhead] = d[i]; tc.rhead = (tc.rhead + 1) % T_RING; }
            tc.rcount += n; tc.rcv_nxt += n;
            if (n < dlen) fl &= ~T_FIN;                 /* tail dropped: FIN not in order */
        } else if ((int32_t)(tc.rcv_nxt - seq) > 0 && (int32_t)(seq + dlen - tc.rcv_nxt) > 0) {
            uint32_t skip = tc.rcv_nxt - seq, i;        /* partial retransmit overlap */
            uint32_t room = T_RING - tc.rcount, n = (dlen - skip) < room ? (dlen - skip) : room;
            for (i = 0; i < n; i++) { tc.ring[tc.rhead] = d[skip + i]; tc.rhead = (tc.rhead + 1) % T_RING; }
            tc.rcount += n; tc.rcv_nxt += n;
            if (n < dlen - skip) fl &= ~T_FIN;
        } else {
            fl &= ~T_FIN;                               /* out of order: dup ACK only */
        }
        need_ack = 1;
    }
    if ((fl & T_FIN) && seq + dlen == tc.rcv_nxt) {
        tc.rcv_nxt++;
        tc.fin = 1;
        if (tc.state == TS_ESTAB) tc.state = TS_CLOSE_WAIT;
        need_ack = 1;
    }
    if (need_ack) tcp_tx(tc.snd_nxt, T_ACK, 0, 0, 0);
}

static int tcp_connect(uint32_t ip, uint16_t port) {
    int tries;
    memset(&tc, 0, sizeof(tc));
    tc.rip = ip; tc.rport = port;
    tc.lport = 49152 + (get_uptime_ms() % 10000);
    uint32_t iss = get_uptime_ms() * 2654435761u + 0x1234567u;
    tc.snd_una = iss; tc.snd_nxt = iss + 1;
    tc.state = TS_SYN_SENT;
    for (tries = 0; tries < 4; tries++) {
        if (tcp_tx(iss, T_SYN, 0, 0, 1) != 0) { tc.state = TS_CLOSED; return NET_E_CONNECT; }
        uint32_t t0 = get_uptime_ms();
        while (get_uptime_ms() - t0 < 1000) {
            net_poll_inner();
            if (tc.state == TS_ESTAB) return 0;
            if (tc.rst) return NET_E_CONNECT;
            asm volatile ("hlt");
        }
    }
    tc.state = TS_CLOSED;
    return NET_E_CONNECT;
}

static int tcp_send(const uint8_t* data, uint32_t len) {
    while (len) {
        uint32_t n = len < tc.mss ? len : tc.mss, seq = tc.snd_nxt;
        int tries, acked = 0;
        tc.snd_nxt += n;
        for (tries = 0; tries < 6 && !acked; tries++) {
            if (tcp_tx(seq, T_PSH | T_ACK, data, n, 0) != 0) return -1;
            uint32_t t0 = get_uptime_ms();
            while (get_uptime_ms() - t0 < 500) {
                net_poll_inner();
                if (tc.rst || tc.state == TS_CLOSED) return -1;
                if ((int32_t)(tc.snd_una - tc.snd_nxt) >= 0) { acked = 1; break; }
                asm volatile ("hlt");
            }
        }
        if (!acked) return -1;
        data += n; len -= n;
    }
    return 0;
}

/* >0 bytes, 0 = clean EOF, -1 = timeout, -2 = reset */
static int tcp_recv(uint8_t* buf, uint32_t cap, int timeout_ms) {
    uint32_t t0 = get_uptime_ms();
    for (;;) {
        net_poll_inner();
        if (tc.rcount) {
            uint32_t before = T_RING - tc.rcount, n = 0;
            while (n < cap && tc.rcount) {
                buf[n++] = tc.ring[tc.rtail];
                tc.rtail = (tc.rtail + 1) % T_RING;
                tc.rcount--;
            }
            if (before < 2920 && T_RING - tc.rcount >= 2920 && tc.state != TS_CLOSED)
                tcp_tx(tc.snd_nxt, T_ACK, 0, 0, 0);          /* window update */
            return (int)n;
        }
        if (tc.fin) return 0;
        if (tc.rst) return -2;
        if ((int)(get_uptime_ms() - t0) >= timeout_ms) return -1;
        asm volatile ("hlt");
    }
}

static void tcp_close(void) {
    if (tc.state == TS_ESTAB || tc.state == TS_CLOSE_WAIT) {
        uint32_t seq = tc.snd_nxt;
        tc.snd_nxt++;
        tcp_tx(seq, T_FIN | T_ACK, 0, 0, 0);
        uint32_t t0 = get_uptime_ms();
        while (get_uptime_ms() - t0 < 500) {
            net_poll_inner();
            if ((int32_t)(tc.snd_una - tc.snd_nxt) >= 0) break;
            asm volatile ("hlt");
        }
    }
    tc.state = TS_CLOSED;
}

/* ------------------------------------------------------------- IP in */

static void ip_in(const uint8_t* p, uint32_t len) {
    if (len < 20 || (p[0] >> 4) != 4) return;
    uint32_t ihl = (p[0] & 0x0F) * 4, tot = rd16(p + 2);
    if (ihl < 20 || tot < ihl || tot > len) return;
    if (rd16(p + 6) & 0x3FFF) return;                       /* fragment: unsupported */
    uint32_t src = rd32(p + 12), dst = rd32(p + 16);
    if (cfg.ip && dst != cfg.ip && dst != 0xFFFFFFFFu) return;
    const uint8_t* pl = p + ihl;
    uint32_t pn = tot - ihl;
    switch (p[9]) {
    case 1:  icmp_in(src, pl, pn); break;
    case 6:  tcp_in(src, pl, pn);  break;
    case 17: udp_in(src, pl, pn);  break;
    }
}

/* -------------------------------------------------------------- poll */

static int last_link;
static uint32_t last_link_check;
static int in_dhcp;

static void net_poll_inner(void) {
    int n, budget = 32;
    if (!nic_ok) return;
    while (budget-- && (n = nic.recv(rxf, sizeof(rxf))) > 0) {
        if (n < 14) continue;
        uint16_t type = rd16(rxf + 12);
        if (type == 0x0806) arp_in(rxf + 14, n - 14);
        else if (type == 0x0800) ip_in(rxf + 14, n - 14);
    }
}

void net_poll(void) {
    if (!nic_ok) return;
    net_poll_inner();
    uint32_t now = get_uptime_ms();
    if (now - last_link_check >= 1000) {
        last_link_check = now;
        int l = nic.link();
        if (l && !last_link && !cfg.ip && !in_dhcp) {       /* cable plugged in later */
            last_link = l;
            net_dhcp();
        }
        last_link = l;
    }
}

/* -------------------------------------------------------------- DHCP */

static int dhcp_build(uint8_t* o, uint32_t xid, int type, uint32_t req_ip, uint32_t srv) {
    int n = 0;
    memset(o, 0, 300);
    o[0] = 1; o[1] = 1; o[2] = 6; o[3] = 0;
    wr32(o + 4, xid);
    wr16(o + 10, 0x8000);                    /* ask for a broadcast reply */
    memcpy(o + 28, nic.mac, 6);
    wr32(o + 236, 0x63825363u);
    n = 240;
    o[n++] = 53; o[n++] = 1; o[n++] = type;
    o[n++] = 61; o[n++] = 7; o[n++] = 1; memcpy(o + n, nic.mac, 6); n += 6;
    if (req_ip) { o[n++] = 50; o[n++] = 4; wr32(o + n, req_ip); n += 4; }
    if (srv)    { o[n++] = 54; o[n++] = 4; wr32(o + n, srv);    n += 4; }
    o[n++] = 55; o[n++] = 3; o[n++] = 1; o[n++] = 3; o[n++] = 6;
    o[n++] = 255;
    return n < 300 ? 300 : n;
}

/* returns message type (2 offer, 5 ack, 6 nak) or 0; fills out params */
static int dhcp_parse(const uint8_t* p, int len, uint32_t xid,
                      uint32_t* yi, uint32_t* srv, uint32_t* mask, uint32_t* gw, uint32_t* dns) {
    if (len < 241 || p[0] != 2 || rd32(p + 4) != xid || rd32(p + 236) != 0x63825363u) return 0;
    if (memcmp(p + 28, nic.mac, 6) != 0) return 0;
    *yi = rd32(p + 16);
    int i = 240, type = 0;
    while (i < len && p[i] != 255) {
        if (p[i] == 0) { i++; continue; }
        if (i + 1 >= len) break;
        int c = p[i], l = p[i + 1];
        if (i + 2 + l > len) break;
        const uint8_t* v = p + i + 2;
        if (c == 53 && l >= 1) type = v[0];
        else if (c == 54 && l >= 4) *srv = rd32(v);
        else if (c == 1 && l >= 4) *mask = rd32(v);
        else if (c == 3 && l >= 4) *gw = rd32(v);
        else if (c == 6 && l >= 4) *dns = rd32(v);
        i += 2 + l;
    }
    return type;
}

static int udp_wait(int timeout_ms) {
    uint32_t t0 = get_uptime_ms();
    while (!us.ready) {
        net_poll_inner();
        if ((int)(get_uptime_ms() - t0) >= timeout_ms) return -1;
        if (!us.ready) asm volatile ("hlt");
    }
    return 0;
}

int net_dhcp(void) {
    uint8_t pkt[320];
    uint32_t xid, yi = 0, srv = 0, mask = 0, gw = 0, dns = 0;
    int attempt, rc = -1;
    if (!nic_ok) return -1;
    in_dhcp = 1;
    cfg.ip = 0; cfg.mask = 0; cfg.gw = 0; cfg.dns = 0;
    memset(arp_tab, 0, sizeof(arp_tab));
    xid = get_uptime_ms() ^ ((nic.mac[4] << 8) | nic.mac[5]) ^ 0x54414E4Au;
    us.port = 68; us.ready = 0;

    for (attempt = 0; attempt < 3 && rc != 0; attempt++) {
        int n = dhcp_build(pkt, xid, 1, 0, 0), got = 0;
        yi = srv = mask = gw = dns = 0;
        udp_send(0xFFFFFFFFu, 68, 67, pkt, n);
        uint32_t t0 = get_uptime_ms();
        while (!got && (int)(get_uptime_ms() - t0) < 2000) {      /* wait for OFFER */
            if (udp_wait(2000 - (int)(get_uptime_ms() - t0)) != 0) break;
            if (dhcp_parse(us.buf, us.len, xid, &yi, &srv, &mask, &gw, &dns) == 2) got = 1;
            us.ready = 0;
        }
        if (!got) continue;

        uint32_t offer = yi, osrv = srv;
        n = dhcp_build(pkt, xid, 3, offer, osrv);
        udp_send(0xFFFFFFFFu, 68, 67, pkt, n);
        t0 = get_uptime_ms();
        while ((int)(get_uptime_ms() - t0) < 2000) {              /* wait for ACK */
            if (udp_wait(2000 - (int)(get_uptime_ms() - t0)) != 0) break;
            int t = dhcp_parse(us.buf, us.len, xid, &yi, &srv, &mask, &gw, &dns);
            us.ready = 0;
            if (t == 5) {
                cfg.ip = yi ? yi : offer;
                cfg.mask = mask ? mask : 0xFFFFFF00u;
                cfg.gw = gw; cfg.dns = dns;
                rc = 0;
                break;
            }
            if (t == 6) break;                                    /* NAK: start over */
        }
    }
    us.port = 0; us.ready = 0;
    in_dhcp = 0;
    return rc;
}

/* --------------------------------------------------------------- DNS */

static int parse_dotted(const char* s, uint32_t* ip) {
    uint32_t v = 0; int parts = 0, cur = -1;
    for (;; s++) {
        if (*s >= '0' && *s <= '9') {
            cur = (cur < 0 ? 0 : cur) * 10 + (*s - '0');
            if (cur > 255) return 0;
        } else if (*s == '.' || *s == 0) {
            if (cur < 0) return 0;
            v = (v << 8) | cur; parts++; cur = -1;
            if (*s == 0) break;
        } else return 0;
    }
    if (parts != 4) return 0;
    *ip = v;
    return 1;
}

static int dns_skip_name(const uint8_t* p, int len, int i) {
    while (i < len) {
        if (p[i] == 0) return i + 1;
        if ((p[i] & 0xC0) == 0xC0) return i + 2;
        i += p[i] + 1;
    }
    return -1;
}

int net_resolve(const char* host, uint32_t* ip) {
    uint8_t q[300];
    int n, attempt, i;
    if (parse_dotted(host, ip)) return 0;
    if (!nic_ok || !cfg.ip || !cfg.dns) return NET_E_NONET;
    int hl = strlen(host);
    if (hl < 1 || hl > 200) return NET_E_DNS;

    uint16_t id = (uint16_t)(get_uptime_ms() ^ 0xA5A5);
    wr16(q, id); wr16(q + 2, 0x0100); wr16(q + 4, 1); wr16(q + 6, 0); wr16(q + 8, 0); wr16(q + 10, 0);
    n = 12;
    {
        const char* s = host;
        while (*s) {
            const char* e = s;
            while (*e && *e != '.') e++;
            int l = e - s;
            if (l < 1 || l > 63) return NET_E_DNS;
            q[n++] = l;
            memcpy(q + n, s, l); n += l;
            s = *e ? e + 1 : e;
        }
        q[n++] = 0;
        wr16(q + n, 1); wr16(q + n + 2, 1); n += 4;       /* A, IN */
    }
    us.port = 40000 + (get_uptime_ms() % 2000); us.ready = 0;
    for (attempt = 0; attempt < 3; attempt++) {
        udp_send(cfg.dns, us.port, 53, q, n);
        uint32_t t0 = get_uptime_ms();
        while ((int)(get_uptime_ms() - t0) < 1500) {
            if (udp_wait(1500 - (int)(get_uptime_ms() - t0)) != 0) break;
            const uint8_t* r = us.buf; int rl = us.len;
            us.ready = 0;
            if (rl < 12 || rd16(r) != id || !(r[2] & 0x80)) continue;
            if ((r[3] & 0x0F) != 0) { us.port = 0; return NET_E_DNS; }     /* NXDOMAIN etc */
            int qd = rd16(r + 4), an = rd16(r + 6), pos = 12;
            for (i = 0; i < qd; i++) { pos = dns_skip_name(r, rl, pos); if (pos < 0) break; pos += 4; }
            for (i = 0; pos > 0 && i < an; i++) {
                pos = dns_skip_name(r, rl, pos);
                if (pos < 0 || pos + 10 > rl) break;
                int type = rd16(r + pos), rdl = rd16(r + pos + 8);
                pos += 10;
                if (pos + rdl > rl) break;
                if (type == 1 && rdl == 4) { *ip = rd32(r + pos); us.port = 0; return 0; }
                pos += rdl;
            }
            us.port = 0;
            return NET_E_DNS;
        }
    }
    us.port = 0;
    return NET_E_DNS;
}

/* -------------------------------------------------------------- ping */

int net_ping(uint32_t ip, int timeout_ms) {
    uint8_t p[40];
    if (!nic_ok || !cfg.ip) return NET_E_NONET;
    memset(p, 0, sizeof(p));
    p[0] = 8;
    wr16(p + 4, PING_ID);
    wr16(p + 6, ++ping_seq);
    {   int i; for (i = 8; i < 40; i++) p[i] = 'a' + (i % 23); }
    wr16(p + 2, csum_fin(csum_add(0, p, 40)));
    ping_got = 0;
    uint32_t t0 = get_uptime_ms();
    if (ip_send(1, ip, p, 40) != 0) return -1;
    while ((int)(get_uptime_ms() - t0) < timeout_ms) {
        net_poll_inner();
        if (ping_got) return (int)(get_uptime_ms() - t0);
        asm volatile ("hlt");
    }
    return -1;
}

/* -------------------------------------------------------------- HTTP */

static int parse_url(const char* url, char* host, int hcap, uint16_t* port, char* path, int pcap, int* tls) {
    int i = 0;
    if (strncasecmp(url, "https://", 8) == 0) { *tls = 1; url += 8; }
    else if (strncasecmp(url, "http://", 7) == 0) { *tls = 0; url += 7; }
    else return NET_E_URL;
    while (*url && *url != '/' && *url != ':' && i < hcap - 1) host[i++] = *url++;
    host[i] = 0;
    if (!i) return NET_E_URL;
    *port = *tls ? 443 : 80;
    if (*url == ':') {
        uint32_t p = 0; url++;
        while (*url >= '0' && *url <= '9') p = p * 10 + (*url++ - '0');
        if (!p || p > 65535) return NET_E_URL;
        *port = (uint16_t)p;
    }
    if (*url != '/' && *url) return NET_E_URL;
    i = 0;
    if (!*url) path[i++] = '/';
    while (*url && i < pcap - 1) path[i++] = *url++;
    path[i] = 0;
    return 0;
}

static int find_hdr_end(const uint8_t* b, uint32_t n) {
    uint32_t i;
    for (i = 0; i + 3 < n; i++)
        if (b[i] == '\r' && b[i + 1] == '\n' && b[i + 2] == '\r' && b[i + 3] == '\n') return i + 4;
    return -1;
}

/* in-place chunked-transfer decode; returns decoded length or -1 */
static int dechunk(uint8_t* b, uint32_t n) {
    uint32_t in = 0, out = 0;
    for (;;) {
        uint32_t sz = 0; int any = 0;
        while (in < n) {
            uint8_t c = b[in];
            int h = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 :
                    (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
            if (h < 0) break;
            sz = sz * 16 + h; any = 1; in++;
        }
        if (!any) return -1;
        while (in < n && b[in] != '\n') in++;
        in++;
        if (sz == 0) return (int)out;
        if (in + sz > n) return -1;
        memmove(b + out, b + in, sz);
        out += sz; in += sz;
        if (in < n && b[in] == '\r') in++;
        if (in < n && b[in] == '\n') in++;
    }
}

/* One connection, plain or TLS: the HTTP code below doesn't care which. */
static int conn_tls;

static int tls_lo_read(unsigned char* d, size_t n) {
    int r = tcp_recv(d, (uint32_t)n, 8000);
    return r > 0 ? r : -1;
}
static int tls_lo_write(const unsigned char* d, size_t n) {
    return tcp_send(d, (uint32_t)n) == 0 ? (int)n : -1;
}
/* >0 bytes, 0 = EOF, -1 = timeout/error, -2 = reset/aborted */
static int conn_recv(uint8_t* b, uint32_t cap) {
    if (!conn_tls) return tcp_recv(b, cap, 8000);
    int r = tls_read(b, cap);
    return r < 0 ? -2 : r;
}
static int conn_send(const uint8_t* b, uint32_t n) {
    return conn_tls ? tls_write_all(b, n) : tcp_send(b, n);
}
static void conn_close(void) {
    if (conn_tls) tls_end();
    tcp_close();
    conn_tls = 0;
}

int net_http_get(const char* url0, uint8_t* buf, uint32_t cap, uint32_t* len) {
    static char url[512], host[128], path[384], loc[384];
    int redirects, rc;
    if (!nic_ok || !cfg.ip) return NET_E_NONET;
    if (strlen(url0) >= (int)sizeof(url)) return NET_E_URL;
    strcpy(url, url0);

    for (redirects = 0; redirects <= 3; redirects++) {
        uint16_t port; uint32_t ip, got = 0;
        char req[640];
        int tls = 0;
        if ((rc = parse_url(url, host, sizeof(host), &port, path, sizeof(path), &tls)) != 0) return rc;
        if (net_resolve(host, &ip) != 0) return NET_E_DNS;
        if (tcp_connect(ip, port) != 0) return NET_E_CONNECT;
        conn_tls = tls;
        if (tls && tls_start(host, tls_lo_read, tls_lo_write) != 0) { conn_close(); return NET_E_TLS; }

        {   /* build the request */
            int n = 0; const char* parts[] = { "GET ", path, " HTTP/1.0\r\nHost: ", host,
                "\r\nUser-Agent: TanjaOS-pkg\r\nAccept: */*\r\nConnection: close\r\n\r\n" };
            int k;
            for (k = 0; k < 5; k++) { int l = strlen(parts[k]); if (n + l >= (int)sizeof(req)) { conn_close(); return NET_E_URL; } memcpy(req + n, parts[k], l); n += l; }
            if (conn_send((uint8_t*)req, n) != 0) { conn_close(); return tls ? NET_E_TLS : NET_E_IO; }
        }

        for (;;) {
            if (got >= cap) { conn_close(); return NET_E_TOOBIG; }
            int r = conn_recv(buf + got, cap - got);
            if (r > 0) { got += r; continue; }
            if (r == 0) break;                          /* EOF */
            if (r == -2 && got) break;                  /* reset / unclean TLS close after data: take what we have */
            conn_close();
            return (tls && !got) ? NET_E_TLS : NET_E_IO;
        }
        conn_close();

        int he = find_hdr_end(buf, got);
        if (he < 0 || got < 12 || memcmp(buf, "HTTP/", 5) != 0) return NET_E_IO;
        int status = 0, i = 9;
        while (buf[i] >= '0' && buf[i] <= '9') status = status * 10 + (buf[i++] - '0');

        int chunked = 0, have_loc = 0;
        uint32_t p = 0;
        while (p < (uint32_t)he) {                      /* headers */
            uint32_t e = p;
            while (e < (uint32_t)he && buf[e] != '\r') e++;
            if (strncasecmp((char*)buf + p, "Transfer-Encoding:", 18) == 0) {
                uint32_t q = p + 18;
                while (q < e && buf[q] == ' ') q++;
                if (strncasecmp((char*)buf + q, "chunked", 7) == 0) chunked = 1;
            } else if (strncasecmp((char*)buf + p, "Location:", 9) == 0) {
                uint32_t q = p + 9; int l = 0;
                while (q < e && buf[q] == ' ') q++;
                while (q < e && l < (int)sizeof(loc) - 1) loc[l++] = buf[q++];
                loc[l] = 0; have_loc = 1;
            }
            p = e + 2;
        }

        if ((status == 301 || status == 302 || status == 303 || status == 307 || status == 308) && have_loc) {
            if (loc[0] == '/') {                        /* same-host redirect */
                char tmp[512]; int n = 0, k;
                const char* pre = tls ? "https://" : "http://";
                for (k = 0; pre[k]; k++) tmp[n++] = pre[k];
                for (k = 0; host[k] && n < 400; k++) tmp[n++] = host[k];
                if (port != (tls ? 443 : 80)) { tmp[n++] = ':'; char d[8]; int dl = 0; uint32_t pv = port; while (pv) { d[dl++] = '0' + pv % 10; pv /= 10; } while (dl) tmp[n++] = d[--dl]; }
                for (k = 0; loc[k] && n < 500; k++) tmp[n++] = loc[k];
                tmp[n] = 0;
                strcpy(url, tmp);
            } else {
                if (strlen(loc) >= (int)sizeof(url)) return NET_E_URL;
                strcpy(url, loc);
            }
            continue;
        }

        uint32_t blen = got - he;
        memmove(buf, buf + he, blen);
        if (chunked) {
            int d = dechunk(buf, blen);
            if (d < 0) return NET_E_IO;
            blen = d;
        }
        *len = blen;
        return status;
    }
    return NET_E_URL;
}

const char* net_tls_error(void) { return tls_error_str(); }

/* ------------------------------------------------------- init / info */

int net_attach(const nic_t* n) {
    nic = *n;
    nic_ok = 1;
    cfg.ip = cfg.mask = cfg.gw = cfg.dns = 0;
    memset(arp_tab, 0, sizeof(arp_tab));
    last_link = nic.link();
    return 0;
}

int net_get_info(net_info_t* o) {
    memset(o, 0, sizeof(*o));
    if (!nic_ok) return 0;
    o->present = 1;
    o->link = nic.link();
    o->configured = cfg.ip != 0;
    strncpy(o->driver, nic.name, sizeof(o->driver) - 1);
    memcpy(o->mac, nic.mac, 6);
    o->ip = cfg.ip; o->mask = cfg.mask; o->gw = cfg.gw; o->dns = cfg.dns;
    return 1;
}

static void log_mac(const char* prefix, const uint8_t* m) {
    static const char hx[] = "0123456789abcdef";
    char b[64]; int n = 0, i;
    while (*prefix) b[n++] = *prefix++;
    for (i = 0; i < 6; i++) {
        b[n++] = hx[m[i] >> 4]; b[n++] = hx[m[i] & 15];
        if (i < 5) b[n++] = ':';
    }
    b[n] = 0;
    boot_log(b);
}

void net_init(void) {
    nic_t n;
    memset(&n, 0, sizeof(n));
    if (nic_probe_pci(&n) != 0) {
        boot_log("Network: no supported Ethernet adapter found");
        return;
    }
    net_attach(&n);
    {
        char b[64] = "Network: ";
        int l = 9, k = 0;
        while (n.name[k]) b[l++] = n.name[k++];
        b[l++] = ' '; b[l] = 0;
        log_mac(b, n.mac);
    }
    /* give auto-negotiation a moment to bring the link up */
    {
        uint32_t t0 = get_uptime_ms();
        while (!nic.link() && get_uptime_ms() - t0 < 2000) timer_delay_ms(50);
    }
    last_link = nic.link();
    if (!last_link) {
        boot_log("Network: cable not connected (will configure when it is plugged in)");
        return;
    }
    boot_log("Network: link up, requesting address via DHCP");
    if (net_dhcp() == 0) {
        char a[16], g[16], msg[96]; int p = 0, k;
        ip_str(cfg.ip, a); ip_str(cfg.gw, g);
        const char* s1 = "Network: "; const char* s2 = "  gateway ";
        for (k = 0; s1[k]; k++) msg[p++] = s1[k];
        for (k = 0; a[k]; k++) msg[p++] = a[k];
        for (k = 0; s2[k]; k++) msg[p++] = s2[k];
        for (k = 0; g[k]; k++) msg[p++] = g[k];
        msg[p] = 0;
        boot_log(msg);
    } else {
        boot_log("Network: DHCP failed (no server answered) - try 'ifconfig dhcp'");
    }
}
