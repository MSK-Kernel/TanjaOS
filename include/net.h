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
} net_info_t;

/* ---- NIC driver ABI (kernel/nic.c, and any future Wi-Fi driver) ---- */
typedef struct {
    const char* name;
    uint8_t     mac[6];
    int  (*send)(const void* frame, uint32_t len);   /* 0 = ok */
    int  (*recv)(void* buf, uint32_t cap);           /* frame length or 0 */
    int  (*link)(void);                              /* 1 = link up */
} nic_t;

int  nic_probe_pci(nic_t* out);   /* kernel/nic.c: 0 = found a supported NIC */

/* ---- kernel-internal ---- */
void net_init(void);              /* probe NIC + DHCP, called once at boot */
void net_poll(void);              /* pump RX; called from idle loops */
int  net_attach(const nic_t* n);  /* make this NIC the active interface */

/* ---- program API (exported to /bin programs) ---- */
int  net_get_info(net_info_t* out);
int  net_dhcp(void);                                   /* 0 = configured */
int  net_ping(uint32_t ip, int timeout_ms);            /* rtt in ms, <0 = no reply */
int  net_resolve(const char* host, uint32_t* ip);      /* 0 = ok; dotted quad or DNS */
const char* net_tls_error(void);                       /* reason for the last NET_E_TLS */

/* GET http(s)://host[:port]/path into buf. Returns the HTTP status (>0) with
 * the body at buf[0..*len), or a negative NET_E_* code. https is TLS 1.2 with
 * the server certificate checked against the built-in Mozilla roots.
 * Follows up to 3 redirects (http or https). */
int  net_http_get(const char* url, uint8_t* buf, uint32_t cap, uint32_t* len);

#define NET_E_NONET   (-1)   /* no NIC / not configured */
#define NET_E_DNS     (-2)
#define NET_E_CONNECT (-3)
#define NET_E_IO      (-4)   /* timeout / reset mid-transfer */
#define NET_E_HTTPS   (-5)   /* unused now: https is supported */
#define NET_E_TLS     (-8)   /* handshake/certificate failure: see net_tls_error() */
#define NET_E_TOOBIG  (-6)
#define NET_E_URL     (-7)

/* ---- Wi-Fi (kernel/wifi.c) ---- */
#define WIFI_SEC_OPEN 0
#define WIFI_SEC_WEP  1
#define WIFI_SEC_WPA  2      /* WPA / WPA2 personal (PSK) */
#define WIFI_SEC_WPA3 3      /* WPA3 personal (SAE), or WPA2/WPA3 mixed */
#define WIFI_SEC_EAP  4      /* 802.1X enterprise */

/* error codes shared by drivers and wifi_* calls (negative) */
#define WIFI_E_NOADAPTER   (-1)
#define WIFI_E_AUTH        (-2)   /* wrong passphrase / rejected */
#define WIFI_E_BADPASS     (-3)   /* passphrase has an invalid length/format */
#define WIFI_E_UNSUPPORTED (-4)   /* security type the driver can't do */
#define WIFI_E_NOTFOUND    (-5)   /* SSID not in range */
#define WIFI_E_TIMEOUT     (-6)
#define WIFI_E_FAIL        (-7)

typedef struct {
    char     ssid[33];
    int      signal;         /* 0..100 */
    int      security;       /* WIFI_SEC_* */
    int      channel;        /* 1-14 = 2.4 GHz, 32+ = 5 GHz */
} wifi_net_t;

/* One wifi_driver_t == one adapter. Drivers only need to fill what their
 * radio can do; scan results may contain duplicates / hidden (empty) SSIDs,
 * the framework tidies them. */
typedef struct {
    const char* name;
    int (*scan)(wifi_net_t* out, int max);                 /* count or <0 */
    int (*connect)(const char* ssid, const char* pass);    /* 0 or WIFI_E_* */
    int (*disconnect)(void);
    int (*status)(char* ssid_out, int cap);                /* 1 = associated */
} wifi_driver_t;

int  wifi_register(const wifi_driver_t* d);            /* kernel-side, up to 4 adapters */
void wifi_init(void);                                  /* boot: autoconnect */

int  wifi_adapter_count(void);
int  wifi_adapter_info(int idx, char* name, int cap);  /* 1 = exists */
int  wifi_select(int idx);                             /* choose the active adapter */
int  wifi_selected(void);
int  wifi_adapter_name(char* out, int cap);            /* active adapter; 1 = present */
int  wifi_scan(wifi_net_t* out, int max);              /* sorted, de-duplicated, no hidden */
int  wifi_connect(const char* ssid, const char* pass); /* "" = open; saves creds on success */
int  wifi_disconnect(void);
int  wifi_status(char* ssid_out, int cap);
int  wifi_saved_pass(const char* ssid, char* pass, int cap);  /* 1 = known */
int  wifi_saved_list(char (*ssids)[33], int max);
int  wifi_forget(const char* ssid);
const char* wifi_strerror(int rc);

#endif
