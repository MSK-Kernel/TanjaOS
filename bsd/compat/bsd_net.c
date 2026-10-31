/* bsd_net.c - network interface glue.
 *
 * The OpenBSD Wi-Fi stack presents an Ethernet-like ifnet. TanjaOS's own stack works
 * on raw Ethernet frames (nic_t.send/recv), so this file bridges the two:
 *   TanjaOS -> radio:  bsd_net_send(frame)  -> mbuf -> if_enqueue -> driver if_start
 *   radio -> TanjaOS:  driver/net80211 if_input(ml) -> queue -> bsd_net_recv(buf)
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <sys/errno.h>
#include <net/if.h>
#include <net/if_var.h>
#include <net/if_dl.h>
#include <net/if_media.h>
#include <net/ifq.h>
#include <netinet/in.h>
#include <netinet/if_ether.h>

extern int printf(const char* fmt, ...);
extern void bsd_poll(void);
extern int bsd_selftest(void);
extern int bsd_mbuf_selftest(void);
extern int bsd_pci_selftest(void);
extern int bsd_wifi_selftest(void);

u_int8_t etheranyaddr[ETHER_ADDR_LEN] = { 0, 0, 0, 0, 0, 0 };
u_int8_t etherbroadcastaddr[ETHER_ADDR_LEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

static struct ifnet *the_ifp;                 /* the (single) wireless interface */
static struct mbuf_list rx_list;
static int rx_init;
static int link_state;

char *ether_sprintf(u_char *ap) {
    static char buf[18];
    static const char hx[] = "0123456789abcdef";
    int i;
    for (i = 0; i < 6; i++) { buf[i * 3] = hx[ap[i] >> 4]; buf[i * 3 + 1] = hx[ap[i] & 15]; buf[i * 3 + 2] = ':'; }
    buf[17] = 0;
    return buf;
}

uint32_t ether_crc32_le_update(uint32_t crc, const uint8_t *buf, size_t len) {
    size_t i; int b;
    for (i = 0; i < len; i++) {
        crc ^= buf[i];
        for (b = 0; b < 8; b++) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
    }
    return crc;
}

int ether_addmulti(struct ifreq *ifr, struct arpcom *ac) { return 0; }
int ether_delmulti(struct ifreq *ifr, struct arpcom *ac) { return 0; }
int ether_ioctl(struct ifnet *ifp, struct arpcom *ac, u_long cmd, caddr_t data) { return 0; }
int ether_output(struct ifnet *ifp, struct mbuf *m, struct sockaddr *dst, struct rtentry *rt) { m_freem(m); return EOPNOTSUPP; }

void ether_ifattach(struct ifnet *ifp) { }
void ether_ifdetach(struct ifnet *ifp) { }

void if_attach(struct ifnet *ifp) {
    ifp->if_snd.ifq_if = ifp;
    ifp->if_snd.ifq_maxlen = 256;
    ml_init(&ifp->if_snd.ifq_free);
    ifp->if_snd.ifq_len = 0;
    the_ifp = ifp;
    if (!rx_init) { ml_init(&rx_list); rx_init = 1; }
}
void if_deactivate(struct ifnet *ifp) { if (the_ifp == ifp) the_ifp = NULL; }
int if_addgroup(struct ifnet *ifp, const char *name) { return 0; }
int if_setlladdr(struct ifnet *ifp, const uint8_t *lladdr) { memcpy(((struct arpcom *)ifp)->ac_enaddr, lladdr, ETHER_ADDR_LEN); return 0; }
void if_link_state_change(struct ifnet *ifp) { link_state = ifp->if_link_state; }
void rtm_80211info(struct ifnet *ifp, struct if_ieee80211_data *ifie) { }

/* send queue: I keep the packets in ifq_free (a plain mbuf_list I own) */
int ifq_enqueue(struct ifqueue *ifq, struct mbuf *m) {
    if (ifq->ifq_len >= ifq->ifq_maxlen) { ifq->ifq_qdrops++; m_freem(m); return ENOBUFS; }
    ml_enqueue(&ifq->ifq_free, m);
    ifq->ifq_len++;
    return 0;
}
struct mbuf *ifq_dequeue(struct ifqueue *ifq) {
    struct mbuf *m = ml_dequeue(&ifq->ifq_free);
    if (m) ifq->ifq_len--;
    return m;
}
void ifq_set_oactive(struct ifqueue *ifq) { ifq->ifq_oactive = 1; }

void if_start(struct ifnet *ifp) { if (ifp->if_start) ifp->if_start(ifp); }

int if_enqueue(struct ifnet *ifp, struct mbuf *m) {
    int err = ifq_enqueue(&ifp->if_snd, m);
    if (err == 0) if_start(ifp);
    return err;
}

/* frames from the radio: queue them for TanjaOS's stack */
void if_input(struct ifnet *ifp, struct mbuf_list *ml) {
    struct mbuf *m;
    while ((m = ml_dequeue(ml)) != NULL) {
        if (rx_list.ml_len >= 64) { m_freem(m); continue; }
        ml_enqueue(&rx_list, m);
    }
}

/* ------------------------------------------- TanjaOS side (nic_t ops) */

int bsd_net_present(void) { return the_ifp != NULL; }
int bsd_net_link(void) { return the_ifp && link_state >= LINK_STATE_UP; }
struct ifnet *bsd_net_ifp(void) { return the_ifp; }

int bsd_net_send(const void *frame, unsigned int len) {
    struct mbuf *m;
    if (!the_ifp || len < 14 || len > 1514) return -1;
    m = m_gethdr(M_DONTWAIT, MT_DATA);
    if (!m) return -1;
    if (len > MHLEN) { m_clget(m, M_DONTWAIT, len); if (!(m->m_flags & M_EXT)) { m_freem(m); return -1; } }
    memcpy(m->m_data, frame, len);
    m->m_len = m->m_pkthdr.len = len;
    return if_enqueue(the_ifp, m) == 0 ? 0 : -1;
}

int bsd_net_recv(void *buf, unsigned int cap) {
    struct mbuf *m;
    int len;
    bsd_poll();
    m = ml_dequeue(&rx_list);
    if (!m) return 0;
    len = m->m_pkthdr.len;
    if ((unsigned int)len > cap) { m_freem(m); return 0; }
    m_copydata(m, 0, len, buf);
    m_freem(m);
    return len;
}

/* ------------------------------------------------------------ selftest */

#define CHK(c, msg) do { if (c) pass++; else { fail++; printf("  FAIL: %s\n", msg); } } while (0)

static int fake_started;
static void fake_start(struct ifnet *ifp) {
    struct mbuf *m;
    while ((m = ifq_dequeue(&ifp->if_snd)) != NULL) {
        struct mbuf_list ml;                   /* loop the frame straight back as a "received" one */
        ml_init(&ml); ml_enqueue(&ml, m); if_input(ifp, &ml);
        fake_started++;
    }
}

int bsd_net_selftest(void) {
    int pass = 0, fail = 0;
    static struct arpcom ac;                   /* a fake interface standing in for a radio */
    static unsigned char frame[200], back[200];
    int i;

    CHK(ether_crc32_le_update(0xffffffff, (const uint8_t*)"123456789", 9) == 0x340bc6d9u, "ether_crc32_le_update (CRC-32 check value)");
    { u_char a[6] = { 0x00, 0x1a, 0x2b, 0x3c, 0x4d, 0x5e }; CHK(memcmp(ether_sprintf(a), "00:1a:2b:3c:4d:5e", 17) == 0, "ether_sprintf"); }

    ac.ac_if.if_start = fake_start;
    if_attach(&ac.ac_if);
    CHK(bsd_net_present(), "if_attach registers the interface");
    for (i = 0; i < 100; i++) frame[i] = (unsigned char)(i + 1);
    CHK(bsd_net_send(frame, 100) == 0 && fake_started == 1, "bsd_net_send -> if_enqueue -> driver if_start");
    CHK(bsd_net_recv(back, sizeof(back)) == 100 && memcmp(back, frame, 100) == 0, "if_input -> bsd_net_recv returns the same frame");
    CHK(bsd_net_recv(back, sizeof(back)) == 0, "recv on empty queue");
    if_deactivate(&ac.ac_if);
    CHK(!bsd_net_present(), "if_deactivate");

    printf("bsd selftest (net): %d passed, %d failed\n", pass, fail);
    return fail;
}

int bsd_selftest_all(void) {
    int f = bsd_selftest();
    f += bsd_mbuf_selftest();
    f += bsd_net_selftest();
    f += bsd_pci_selftest();
    f += bsd_wifi_selftest();
    printf("bsd selftest: %s\n", f ? "FAILED" : "all passed");
    return f;
}
