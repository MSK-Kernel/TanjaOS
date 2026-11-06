#ifndef TANJA_NET_H
#define TANJA_NET_H

#include <stdint.h>

/* IPv4 addresses are plain host-order integers: 10.0.2.15 == 0x0A00020F */
#define IP4(a,b,c,d) ((((uint32_t)(a)) << 24) | (((uint32_t)(b)) << 16) | \
                      (((uint32_t)(c)) << 8)  |  ((uint32_t)(d)))

typedef struct {
    int      present;       /* a NIC driver is attached */
    int      link;          /* cable / association is up */
    int      configured;    /* has an IPv4 address */
    char     driver[16];    /* "e1000", "rtl8139", ... */
    uint8_t  mac[6];
    uint32_t ip, mask, gw, dns;
    uint32_t rx_frames, tx_frames, tx_errors;   /* since boot / last switch */
} net_info_t;

/* ---- NIC driver ABI (kernel/nic.c, and any future Wi-Fi driver) ---- */
typedef struct {
    const char* name;
    uint8_t     mac[6];
    int  (*send)(const void* frame, uint32_t len);   /* 0 = ok */
    int  (*recv)(void* buf, uint32_t cap);           /* frame length or 0 */
    int  (*link)(void);                              /* 1 = link up */
} nic_t;

/* One PCI Ethernet controller found at boot */
#define NIC_ST_READY    0   /* driver attached and running */
#define NIC_ST_NODRIVER 1   /* a network card we have no driver for (yet) */
#define NIC_ST_FAILED   2   /* we have a driver but it could not start the card */
#define NIC_ST_SECOND   3   /* second card of a kind that is already in use */
typedef struct {
    nic_t       nic;                 /* valid when state == NIC_ST_READY */
    const char* driver;              /* driver name, or 0 */
    uint16_t    vendor, device;
    uint8_t     bus, dev, fn;
    int         state;
} nic_found_t;
int  nic_scan_all(nic_found_t* out, int max);   /* kernel/nic.c: find + start every Ethernet card */

/* ---- kernel-internal ---- */
void net_init(void);              /* probe NIC + DHCP, called once at boot */
void net_poll(void);              /* pump RX; called from idle loops */
int  net_attach(const nic_t* n);  /* make this NIC the active interface */

/* ---- program API (exported to /bin programs) ---- */
int  net_get_info(net_info_t* out);
int  net_dhcp(void);                                   /* 0 = configured */

/* every Ethernet controller seen at boot, usable or not */
typedef struct {
    char     name[16];                 /* driver name, or "" when there is none */
    uint16_t vendor, device;
    uint8_t  bus, dev, fn;
    uint8_t  mac[6];
    int      state;                    /* NIC_ST_* */
    int      link;                     /* 1 = cable/link up (READY cards only) */
    int      active;                   /* this is the interface the stack uses */
} net_nic_info_t;
int  net_nic_count(void);
int  net_nic_info(int idx, net_nic_info_t* out);       /* 0 = ok */
int  net_nic_use(int idx);                             /* switch the active interface (not configured yet) */
int  net_set_static(uint32_t ip, uint32_t mask, uint32_t gw, uint32_t dns);
int  net_ping(uint32_t ip, int timeout_ms);            /* rtt in ms, <0 = no reply */
int  net_resolve(const char* host, uint32_t* ip);      /* 0 = ok; dotted quad or DNS */
const char* net_tls_error(void);                       /* reason for the last NET_E_TLS */

/* GET http(s)://host[:port]/path into buf. Returns the HTTP status (>0) with
 * the body at buf[0..*len), or a negative NET_E_* code. https is TLS 1.2 with
 * the server certificate checked against the built-in Mozilla roots.
 * Follows up to 3 redirects (http or https). */
int  net_http_get(const char* url, uint8_t* buf, uint32_t cap, uint32_t* len);
int  net_http_save(const char* url, const char* path, uint32_t* len);

#define NET_E_NONET   (-1)   /* no NIC / not configured */
#define NET_E_DNS     (-2)
#define NET_E_CONNECT (-3)
#define NET_E_IO      (-4)   /* timeout / reset mid-transfer */
#define NET_E_HTTPS   (-5)   /* unused now: https is supported */
#define NET_E_TLS     (-8)   /* handshake/certificate failure: see net_tls_error() */
#define NET_E_TOOBIG  (-6)
#define NET_E_URL     (-7)

#endif
