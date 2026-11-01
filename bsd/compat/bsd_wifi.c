/* bsd_wifi.c - attaches OpenBSD's iwm driver (Intel Wireless-AC) to a real card and drives it
 * the same way ifconfig(8) would: through net80211's ioctls.
 *
 * NOT run automatically at boot (a driver bug could hang boot): the user starts it with
 * `wifi attach`, and Ctrl+C aborts it.
 *
 * Plain C types at the boundary (kernel/wifi.c does the translation to wifi_net_t etc.).
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <sys/device.h>
#include <sys/sockio.h>
#include <net/if.h>
#include <net/if_var.h>
#include <net/if_media.h>
#include <netinet/in.h>
#include <netinet/if_ether.h>
#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_ioctl.h>
#include <net80211/ieee80211_crypto.h>
#include <machine/bus.h>
#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>

extern int printf(const char *fmt, ...);
extern void bsd_poll(void);
extern unsigned int get_uptime_ms(void);

struct hw { unsigned short vendor, device; unsigned char bus, dev, fn; };
extern int nic_wifi_scan(struct hw *out, int max);                 /* kernel/nic.c */

/* the NIC descriptor TanjaOS's IP stack takes (layout of nic_t in include/net.h) */
struct tnic {
    const char *name;
    unsigned char mac[6];
    int (*send)(const void *frame, unsigned int len);
    int (*recv)(void *buf, unsigned int cap);
    int (*link)(void);
};
extern int net_attach(const struct tnic *n);

extern struct cfattach iwm_ca;                                      /* from if_iwm.c */
extern struct bus_dma_tag bsd_dma_tag;
extern int bsd_net_send(const void *frame, unsigned int len);
extern int bsd_net_recv(void *buf, unsigned int cap);
extern int bsd_net_link(void);
extern struct ifnet *bsd_net_ifp(void);

static struct ieee80211com *ic;
static struct ifnet *ifp;
static int attached;
static char drv_name[48] = "Intel Wireless-AC (OpenBSD iwm)";
static struct tnic tnic;

int bsd_wifi_attached(void) { return attached; }

/* does the driver claim these PCI ids? (checks the real iwm_match) */
static int claims(unsigned short ven, unsigned short dev) {
    struct pci_attach_args pa;
    memset(&pa, 0, sizeof(pa));
    pa.pa_id = ((unsigned)dev << 16) | ven;
    return iwm_ca.ca_match(NULL, NULL, &pa);
}
int bsd_wifi_selftest(void) {
    int pass = 0, fail = 0;
#define CHK(c, msg) do { if (c) pass++; else { fail++; printf("  FAIL: %s\n", msg); } } while (0)
    CHK(claims(0x8086, 0x2526), "iwm claims 8086:2526 (Wireless-AC 9260)");
    CHK(claims(0x8086, 0x24fd), "iwm claims 8086:24fd (Wireless-AC 8265)");
    CHK(claims(0x8086, 0x095a), "iwm claims 8086:095a (Wireless-AC 7265)");
    CHK(!claims(0x8086, 0x2723), "iwm refuses 8086:2723 (AX200 needs the newer iwx driver)");
    CHK(!claims(0x10ec, 0x8168), "iwm refuses a Realtek id");
    printf("bsd selftest (iwm match): %d passed, %d failed\n", pass, fail);
    return fail;
}
const char *bsd_wifi_name(void) { return drv_name; }

/* ------------------------------------------------------------ attach */

/* returns 0 on success; msg describes the failure */
int bsd_wifi_attach(char *msg, int cap) {
    struct hw hw[8];
    int n = nic_wifi_scan(hw, 8), i;
    struct pci_attach_args pa;
    struct device *dv;
    void *sc;

    if (attached) { snprintf(msg, cap, "already attached"); return 0; }
    for (i = 0; i < n; i++) {
        memset(&pa, 0, sizeof(pa));
        pa.pa_pc = NULL;
        pa.pa_iot = I386_BUS_SPACE_IO;
        pa.pa_memt = I386_BUS_SPACE_MEM;
        pa.pa_dmat = &bsd_dma_tag;
        pa.pa_tag.mode1 = 0x80000000u | ((unsigned)hw[i].bus << 16) | ((unsigned)hw[i].dev << 11) | ((unsigned)hw[i].fn << 8);
        pa.pa_bus = hw[i].bus; pa.pa_device = hw[i].dev; pa.pa_function = hw[i].fn;
        pa.pa_id = ((unsigned)hw[i].device << 16) | hw[i].vendor;
        pa.pa_class = pci_conf_read(NULL, pa.pa_tag, PCI_CLASS_REG);
        if (iwm_ca.ca_match(NULL, NULL, &pa) == 0) continue;

        sc = malloc(iwm_ca.ca_devsize, M_DEVBUF, M_NOWAIT | M_ZERO);
        if (!sc) { snprintf(msg, cap, "out of memory"); return -1; }
        dv = (struct device *)sc;
        strlcpy(dv->dv_xname, "iwm0", sizeof(dv->dv_xname));
        printf("iwm0: attaching to %04x:%04x\n", hw[i].vendor, hw[i].device);
        iwm_ca.ca_attach(NULL, dv, &pa);

        ifp = bsd_net_ifp();
        if (!ifp) {
            snprintf(msg, cap, "Intel card found but driver did not start — copy the matching iwlwifi-*.ucode into home/lib/firmware and rebuild");
            return -2;
        }
        ic = (struct ieee80211com *)ifp;
        attached = 1;
        snprintf(msg, cap, "iwm0 attached, MAC %s", ether_sprintf(ic->ic_myaddr));
        return 0;
    }
    snprintf(msg, cap, "no supported Intel Wireless-AC card (3160-9560). AX200+ and Realtek need a different driver");
    return -3;
}

/* ------------------------------------------------------------ control */

static int ifup(void) {
    int err;
    if ((ifp->if_flags & (IFF_UP | IFF_RUNNING)) == (IFF_UP | IFF_RUNNING)) return 0;
    ifp->if_flags |= IFF_UP;
    err = ifp->if_ioctl(ifp, SIOCSIFFLAGS, NULL);
    return err;
}

static void wait_ms(unsigned int ms) {
    unsigned int t0 = get_uptime_ms();
    while (get_uptime_ms() - t0 < ms) { bsd_poll(); __asm volatile("hlt"); }
}

/* fills up to max entries; security: 0 open, 1 WEP, 2 WPA/WPA2 personal, 3 WPA3, 4 enterprise */
int bsd_wifi_scan(char (*ssid)[33], int *signal, int *sec, int *chan, int max) {
    struct ieee80211_node *ni;
    struct ifreq ifr;
    int n = 0;
    if (!attached) return -1;
    if (ifup() != 0) return -1;
    memset(&ifr, 0, sizeof(ifr));
    ifp->if_ioctl(ifp, SIOCS80211SCAN, (caddr_t)&ifr);
    wait_ms(4000);
    RBT_FOREACH(ni, ieee80211_tree, &ic->ic_tree) {
        int len = ni->ni_esslen, s, r = ni->ni_rssi;
        if (n >= max) break;
        if (len > 32) len = 32;
        memcpy(ssid[n], ni->ni_essid, len); ssid[n][len] = 0;
        r = (int)(signed char)ni->ni_rssi;               /* dBm */
        s = (r + 100) * 100 / 60;
        signal[n] = s < 0 ? 0 : s > 100 ? 100 : s;
        if (ni->ni_rsnprotos & (IEEE80211_PROTO_RSN | IEEE80211_PROTO_WPA)) {
            if ((ni->ni_rsnakms & IEEE80211_AKM_SAE) && !(ni->ni_rsnakms & IEEE80211_AKM_PSK)) sec[n] = 3;
            else if ((ni->ni_rsnakms & IEEE80211_AKM_8021X) && !(ni->ni_rsnakms & IEEE80211_AKM_PSK)) sec[n] = 4;
            else sec[n] = 2;
        } else if (ni->ni_capinfo & IEEE80211_CAPINFO_PRIVACY) sec[n] = 1;
        else sec[n] = 0;
        chan[n] = ni->ni_chan ? ieee80211_mhz2ieee(ni->ni_chan->ic_freq, ni->ni_chan->ic_flags) : 0;
        n++;
    }
    return n;
}

/* pbkdf2: from kernel/wpa.c */
extern void wpa_pmk_from_passphrase(const char *pass, const char *ssid, unsigned char pmk[32]);

/* 0 ok, else WIFI_E_* from include/net.h: -2 auth, -4 unsupported, -5 not found, -6 timeout, -7 fail */
int bsd_wifi_connect(const char *ssid, const char *pass) {
    struct ieee80211_nwid nwid;
    struct ieee80211_wpaparams wpa;
    struct ieee80211_wpapsk psk;
    unsigned int t0;
    int err, secured = pass[0] != 0, len = strlen(ssid);
    if (!attached) return -1;

    memset(&nwid, 0, sizeof(nwid));
    nwid.i_len = len; memcpy(nwid.i_nwid, ssid, len);
    memset(&wpa, 0, sizeof(wpa));
    memset(&psk, 0, sizeof(psk));
    if (secured) {
        int plen = strlen(pass);
        if (plen == 5 || plen == 13) return -4;            /* WEP: not wired up yet */
        wpa.i_enabled = 1;
        wpa.i_protos = IEEE80211_WPA_PROTO_WPA2;
        wpa.i_akms = IEEE80211_WPA_AKM_PSK;
        wpa.i_ciphers = IEEE80211_WPA_CIPHER_CCMP;
        wpa.i_groupcipher = IEEE80211_WPA_CIPHER_CCMP;
        psk.i_enabled = 1;
        wpa_pmk_from_passphrase(pass, ssid, psk.i_psk);
    }
    err = ifp->if_ioctl(ifp, SIOCS80211WPAPARMS, (caddr_t)&wpa);
    if (err && err != ENETRESET) return -7;
    err = ifp->if_ioctl(ifp, SIOCS80211WPAPSK, (caddr_t)&psk);
    if (err && err != ENETRESET) return -7;
    err = ifp->if_ioctl(ifp, SIOCS80211NWID, (caddr_t)&nwid);
    if (err && err != ENETRESET) return -7;
    if (ifup() != 0) return -7;
    if (err == ENETRESET && (ifp->if_flags & IFF_RUNNING)) {      /* settings changed on a running interface */
        ifp->if_flags &= ~IFF_UP; ifp->if_ioctl(ifp, SIOCSIFFLAGS, NULL);
        ifp->if_flags |= IFF_UP;  ifp->if_ioctl(ifp, SIOCSIFFLAGS, NULL);
    }

    t0 = get_uptime_ms();
    while (get_uptime_ms() - t0 < 20000) {
        bsd_poll();
        if (ic->ic_state == IEEE80211_S_RUN) break;
        __asm volatile("hlt");
    }
    if (ic->ic_state != IEEE80211_S_RUN) return secured ? -2 : -6;

    /* link is up: hand the radio to TanjaOS's IP stack as its network interface */
    memset(&tnic, 0, sizeof(tnic));
    tnic.name = "iwm";
    memcpy(tnic.mac, ic->ic_myaddr, 6);
    tnic.send = bsd_net_send; tnic.recv = bsd_net_recv; tnic.link = bsd_net_link;
    net_attach(&tnic);
    return 0;
}

int bsd_wifi_disconnect(void) {
    if (!attached) return -1;
    ifp->if_flags &= ~IFF_UP;
    ifp->if_ioctl(ifp, SIOCSIFFLAGS, NULL);
    return 0;
}

int bsd_wifi_status(char *out, int cap) {
    if (!attached || ic->ic_state != IEEE80211_S_RUN || !ic->ic_bss) { if (cap) out[0] = 0; return 0; }
    {
        int len = ic->ic_bss->ni_esslen;
        if (len > cap - 1) len = cap - 1;
        memcpy(out, ic->ic_bss->ni_essid, len); out[len] = 0;
    }
    return 1;
}
