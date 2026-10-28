/* wifi.c - Wi-Fi framework for TanjaOS.
 *
 * This is the layer a radio driver plugs into. It handles everything that
 * is the same for every chipset: the adapter registry (up to 4), passphrase
 * validation, tidying scan results (sorted by signal, duplicates merged,
 * hidden / non-printable SSIDs cleaned), the saved-network list
 * (/etc/wifi.conf, one "ssid<TAB>pass" per line, root-only) and the
 * boot-time autoconnect to the strongest saved network.
 *
 * There is NO real radio driver in the tree yet: a chipset driver needs its
 * firmware, the 802.11 management frames and WPA2/WPA3 (PBKDF2 + handshake +
 * CCMP). A driver finishes the job by calling wifi_register() with its ops
 * and net_attach() with its data path; DHCP/IP/TLS then work over it as is.
 *
 * `make WIFI_DEMO=1` adds two fake adapters with invented networks (WEP,
 * WPA2, WPA3, enterprise, duplicates, hidden, odd names, a long list) for
 * trying the menu out. Demo passphrase for secured networks: demo1234.
 */
#include <stdint.h>
#include "../include/net.h"
#include "../include/fs.h"

extern void  boot_log(const char* msg);
extern int   strlen(const char* s);
extern int   strcmp(const char* a, const char* b);
extern int   strncmp(const char* a, const char* b, unsigned int n);
extern char* strncpy(char* d, const char* s, unsigned int n);
extern void* memcpy(void* d, const void* s, unsigned int n);
extern int   fs_cur_uid;

#define MAX_AD 4
static const wifi_driver_t* drv[MAX_AD];
static int nad, sel;

int wifi_register(const wifi_driver_t* d) {
    if (nad >= MAX_AD) return -1;
    drv[nad++] = d;
    return 0;
}

static const wifi_driver_t* cur(void) { return nad ? drv[sel] : 0; }

int wifi_adapter_count(void) { return nad; }
int wifi_selected(void) { return sel; }

int wifi_adapter_info(int idx, char* name, int cap) {
    if (idx < 0 || idx >= nad) { if (cap) name[0] = 0; return 0; }
    strncpy(name, drv[idx]->name, cap - 1); name[cap - 1] = 0;
    return 1;
}

int wifi_select(int idx) {
    if (idx < 0 || idx >= nad) return -1;
    sel = idx;
    return 0;
}

int wifi_adapter_name(char* out, int cap) { return wifi_adapter_info(sel, out, cap); }

const char* wifi_strerror(int rc) {
    switch (rc) {
    case 0:                   return "ok";
    case WIFI_E_NOADAPTER:    return "no Wi-Fi adapter";
    case WIFI_E_AUTH:         return "wrong password (or rejected by the network)";
    case WIFI_E_BADPASS:      return "invalid password (WPA needs 8-63 characters or 64 hex digits)";
    case WIFI_E_UNSUPPORTED:  return "this security type is not supported by the adapter driver";
    case WIFI_E_NOTFOUND:     return "network not in range";
    case WIFI_E_TIMEOUT:      return "timed out";
    }
    return "connection failed";
}

/* ------------------------------------------------------ saved networks */

#define CONF "/etc/wifi.conf"
#define CONF_MAX 4096
static char conf[CONF_MAX];

static int conf_load(void) {
    uint32_t n = CONF_MAX - 1;
    if (!fs_file_exists(CONF) || fs_read_file(CONF, conf, &n) != 0) { conf[0] = 0; return 0; }
    conf[n] = 0;
    return 1;
}

static int conf_save(void) {
    int old = fs_cur_uid, rc;
    fs_cur_uid = 0;                                   /* credentials are root-owned */
    if (!fs_directory_exists("/etc")) fs_create_directory("/etc");
    rc = fs_write_file(CONF, conf, strlen(conf));
    if (rc == 0) fs_chmod(CONF, 0600);
    fs_cur_uid = old;
    return rc;
}

static int conf_find(const char* ssid, int* line_len) {
    int i = 0, sl = strlen(ssid);
    while (conf[i]) {
        int e = i;
        while (conf[e] && conf[e] != '\n') e++;
        if (e - i > sl && strncmp(conf + i, ssid, sl) == 0 && conf[i + sl] == '\t') {
            *line_len = e - i + (conf[e] == '\n');
            return i;
        }
        i = e + (conf[e] == '\n');
    }
    return -1;
}

int wifi_saved_pass(const char* ssid, char* pass, int cap) {
    int ll, at;
    conf_load();
    at = conf_find(ssid, &ll);
    if (at < 0) return 0;
    {
        const char* p = conf + at + strlen(ssid) + 1;
        int n = 0;
        while (*p && *p != '\n' && n < cap - 1) pass[n++] = *p++;
        pass[n] = 0;
    }
    return 1;
}

int wifi_saved_list(char (*ssids)[33], int max) {
    int i = 0, n = 0;
    conf_load();
    while (conf[i] && n < max) {
        int e = i, k = 0;
        while (conf[e] && conf[e] != '\n') e++;
        while (i + k < e && conf[i + k] != '\t' && k < 32) { ssids[n][k] = conf[i + k]; k++; }
        ssids[n][k] = 0;
        if (k) n++;
        i = e + (conf[e] == '\n');
    }
    return n;
}

int wifi_forget(const char* ssid) {
    int ll, at, i;
    conf_load();
    at = conf_find(ssid, &ll);
    if (at < 0) return -1;
    for (i = at; ; i++) { conf[i] = conf[i + ll]; if (!conf[i]) break; }
    return conf_save();
}

static void conf_remember(const char* ssid, const char* pass) {
    int ll, at, n;
    conf_load();
    at = conf_find(ssid, &ll);
    if (at >= 0) {
        int i;
        for (i = at; ; i++) { conf[i] = conf[i + ll]; if (!conf[i]) break; }
    }
    n = strlen(conf);
    if (n + strlen(ssid) + strlen(pass) + 3 >= CONF_MAX) return;
    memcpy(conf + n, ssid, strlen(ssid)); n += strlen(ssid);
    conf[n++] = '\t';
    memcpy(conf + n, pass, strlen(pass)); n += strlen(pass);
    conf[n++] = '\n'; conf[n] = 0;
    conf_save();
}

/* --------------------------------------------------------------- scan */

#define RAW_MAX 64
static wifi_net_t raw[RAW_MAX], res[RAW_MAX];

int wifi_scan(wifi_net_t* out, int max) {
    const wifi_driver_t* d = cur();
    int n, i, j, m = 0;
    if (!d) return WIFI_E_NOADAPTER;
    n = d->scan(raw, RAW_MAX);
    if (n < 0) return n;
    if (n > RAW_MAX) n = RAW_MAX;
    for (i = 0; i < n; i++) {
        wifi_net_t* r = &raw[i];
        int dup = -1;
        r->ssid[32] = 0;
        for (j = 0; r->ssid[j]; j++)                         /* printable ASCII only */
            if ((unsigned char)r->ssid[j] < 32 || (unsigned char)r->ssid[j] > 126) r->ssid[j] = '?';
        if (!r->ssid[0]) continue;                           /* hidden network */
        if (r->signal < 0) r->signal = 0;
        if (r->signal > 100) r->signal = 100;
        for (j = 0; j < m; j++) if (strcmp(res[j].ssid, r->ssid) == 0) { dup = j; break; }
        if (dup >= 0) { if (r->signal > res[dup].signal) res[dup] = *r; }
        else res[m++] = *r;
    }
    for (i = 1; i < m; i++) {                                /* strongest first */
        wifi_net_t t = res[i];
        for (j = i - 1; j >= 0 && res[j].signal < t.signal; j--) res[j + 1] = res[j];
        res[j + 1] = t;
    }
    if (m > max) m = max;
    for (i = 0; i < m; i++) out[i] = res[i];
    return m;
}

/* ------------------------------------------------------------ connect */

/* "" = open network; else WPA (8-63 chars or 64 hex) or WEP (5/13 chars, 10/26 hex) */
static int valid_pass(const char* p) {
    int n = strlen(p), i, hex = 1;
    if (!n) return 1;
    for (i = 0; i < n; i++) {
        unsigned char c = p[i];
        if (c < 32 || c > 126) return 0;
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) hex = 0;
    }
    if (n >= 8 && n <= 63) return 1;
    if (n == 64 && hex) return 1;
    if (n == 5 || n == 13) return 1;
    if ((n == 10 || n == 26) && hex) return 1;
    return 0;
}

int wifi_connect(const char* ssid, const char* pass) {
    const wifi_driver_t* d = cur();
    int rc;
    if (!d) return WIFI_E_NOADAPTER;
    if (!ssid[0] || strlen(ssid) > 32) return WIFI_E_NOTFOUND;
    if (!valid_pass(pass)) return WIFI_E_BADPASS;
    rc = d->connect(ssid, pass);
    if (rc != 0) return rc < 0 ? rc : WIFI_E_FAIL;
    conf_remember(ssid, pass);
    net_dhcp();                                   /* only works once the driver attached its data path */
    return 0;
}

int wifi_disconnect(void) { const wifi_driver_t* d = cur(); return d ? d->disconnect() : WIFI_E_NOADAPTER; }
int wifi_status(char* s, int cap) {
    const wifi_driver_t* d = cur();
    if (cap) s[0] = 0;
    return d ? d->status(s, cap) : 0;
}

/* boot: reconnect to the strongest saved network in range, trying each adapter */
void wifi_init(void) {
    static wifi_net_t found[32];
    int a, n, i, saved_sel = sel;
    char pass[80];
    net_info_t ni;
#ifdef WIFI_DEMO
    {
        extern void wifi_demo_register(void);
        wifi_demo_register();
    }
#endif
    if (!nad) { boot_log("Wi-Fi: no supported adapter (wired only)"); return; }
    boot_log(nad == 1 ? "Wi-Fi: 1 adapter found" : "Wi-Fi: several adapters found");
    net_get_info(&ni);
    if (ni.present && ni.configured) return;       /* wired already works */
    for (a = 0; a < nad; a++) {
        sel = a;
        n = wifi_scan(found, 32);
        for (i = 0; i < n; i++) {
            if (!wifi_saved_pass(found[i].ssid, pass, sizeof(pass))) continue;
            if (found[i].security == WIFI_SEC_EAP) continue;
            if (drv[a]->connect(found[i].ssid, pass) == 0) {
                boot_log("Wi-Fi: connected to a saved network");
                net_dhcp();
                return;
            }
        }
    }
    sel = saved_sel;
}

#ifdef WIFI_DEMO
/* ---- demo adapters: no radio, no data path, invented networks ---- */
typedef struct { const char* s; int sig, sec, ch; } dnet;

static const dnet demo0[] = {
    { "HomeNet-5G (demo)",      88, WIFI_SEC_WPA,  36 },
    { "HomeNet-5G (demo)",      52, WIFI_SEC_WPA,   6 },   /* same SSID, weaker AP */
    { "CoffeeShop Guest (demo)",61, WIFI_SEC_OPEN,  6 },
    { "NextGen-WPA3 (demo)",    70, WIFI_SEC_WPA3, 149 },
    { "Legacy-WEP (demo)",      44, WIFI_SEC_WEP,   1 },
    { "Corp-8021X (demo)",      39, WIFI_SEC_EAP,  11 },
    { "",                       30, WIFI_SEC_WPA,   3 },   /* hidden */
    { "caf\xc3\xa9 \x01wifi",   25, WIFI_SEC_OPEN, 13 },   /* odd bytes */
    { "Neighbor-01 (demo)", 34, WIFI_SEC_WPA, 1 }, { "Neighbor-02 (demo)", 33, WIFI_SEC_WPA, 6 },
    { "Neighbor-03 (demo)", 32, WIFI_SEC_WPA, 11 }, { "Neighbor-04 (demo)", 31, WIFI_SEC_WPA, 36 },
    { "Neighbor-05 (demo)", 29, WIFI_SEC_WPA, 40 }, { "Neighbor-06 (demo)", 28, WIFI_SEC_WPA, 44 },
    { "Neighbor-07 (demo)", 27, WIFI_SEC_WPA, 48 }, { "Neighbor-08 (demo)", 26, WIFI_SEC_WPA, 52 },
    { "Neighbor-09 (demo)", 24, WIFI_SEC_WPA, 1 },  { "Neighbor-10 (demo)", 23, WIFI_SEC_WPA, 6 },
};
static const dnet demo1[] = {
    { "USB-Dongle-Net (demo)",  77, WIFI_SEC_WPA,   2 },
    { "Garage (demo)",          35, WIFI_SEC_OPEN,  9 },
};
static char demo_cur[2][33];

static int demo_scan_i(int i, wifi_net_t* out, int max) {
    const dnet* t = i ? demo1 : demo0;
    int n = i ? (int)(sizeof(demo1) / sizeof(demo1[0])) : (int)(sizeof(demo0) / sizeof(demo0[0])), k;
    if (n > max) n = max;
    for (k = 0; k < n; k++) {
        strncpy(out[k].ssid, t[k].s, 32); out[k].ssid[32] = 0;
        out[k].signal = t[k].sig; out[k].security = t[k].sec; out[k].channel = t[k].ch;
    }
    return n;
}

static int demo_connect_i(int i, const char* ssid, const char* pass) {
    const dnet* t = i ? demo1 : demo0;
    int n = i ? (int)(sizeof(demo1) / sizeof(demo1[0])) : (int)(sizeof(demo0) / sizeof(demo0[0])), k, sec = -1, pl = strlen(pass);
    for (k = 0; k < n; k++) if (t[k].s[0] && strcmp(t[k].s, ssid) == 0) { sec = t[k].sec; break; }
    if (sec < 0 && !i && strcmp(ssid, "HiddenNet (demo)") == 0) sec = WIFI_SEC_WPA;   /* hidden, not in scans */
    if (sec < 0) return WIFI_E_NOTFOUND;
    if (sec == WIFI_SEC_EAP) return WIFI_E_UNSUPPORTED;
    if (sec == WIFI_SEC_WEP && pl != 5 && pl != 13 && pl != 10 && pl != 26) return WIFI_E_AUTH;
    if ((sec == WIFI_SEC_WPA || sec == WIFI_SEC_WPA3) && strcmp(pass, "demo1234") != 0) return WIFI_E_AUTH;
    if (sec == WIFI_SEC_OPEN && pl) return WIFI_E_AUTH;
    strncpy(demo_cur[i], ssid, 32);
    return 0;
}

static int demo_status_i(int i, char* s, int cap) {
    if (!demo_cur[i][0]) return 0;
    strncpy(s, demo_cur[i], cap - 1); s[cap - 1] = 0;
    return 1;
}

#define DEMO_ADAPTER(I, NAME) \
    static int ds##I(wifi_net_t* o, int m) { return demo_scan_i(I, o, m); } \
    static int dc##I(const char* s, const char* p) { return demo_connect_i(I, s, p); } \
    static int dd##I(void) { demo_cur[I][0] = 0; return 0; } \
    static int dt##I(char* s, int c) { return demo_status_i(I, s, c); } \
    static const wifi_driver_t demo_drv##I = { NAME, ds##I, dc##I, dd##I, dt##I };

DEMO_ADAPTER(0, "demo0 - no radio, no data path")
DEMO_ADAPTER(1, "demo1 - fake USB dongle")

void wifi_demo_register(void) { wifi_register(&demo_drv0); wifi_register(&demo_drv1); }
#endif
