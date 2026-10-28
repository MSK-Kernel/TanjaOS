/* nic.c - PCI NIC drivers for TanjaOS (polled, no interrupts).
 *
 * Supported: Intel PRO/1000 family (e1000 / e1000e legacy descriptors,
 * the default NIC of QEMU) and Realtek RTL8139. Everything runs in
 * ring 0 with no paging, so a buffer's address IS its DMA address.
 */
#include <stdint.h>
#include "../include/net.h"

extern void  timer_delay_ms(uint32_t ms);
extern uint32_t get_uptime_ms(void);
extern void* memset(void* d, int v, unsigned int n);
extern void* memcpy(void* d, const void* s, unsigned int n);

static inline void     io_outb(uint16_t p, uint8_t v)  { asm volatile ("outb %0, %1" : : "a"(v), "Nd"(p)); }
static inline void     io_outw(uint16_t p, uint16_t v) { asm volatile ("outw %0, %1" : : "a"(v), "Nd"(p)); }
static inline void     io_outl(uint16_t p, uint32_t v) { asm volatile ("outl %0, %1" : : "a"(v), "Nd"(p)); }
static inline uint8_t  io_inb(uint16_t p)  { uint8_t v;  asm volatile ("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline uint16_t io_inw(uint16_t p)  { uint16_t v; asm volatile ("inw %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline uint32_t io_inl(uint16_t p)  { uint32_t v; asm volatile ("inl %1, %0" : "=a"(v) : "Nd"(p)); return v; }

/* ---------------------------------------------------------------- PCI */

static uint32_t pci_rd(uint8_t b, uint8_t d, uint8_t f, uint8_t off) {
    io_outl(0xCF8, 0x80000000u | ((uint32_t)b << 16) | ((uint32_t)d << 11) |
                   ((uint32_t)f << 8) | (off & 0xFC));
    return io_inl(0xCFC);
}

static void pci_wr(uint8_t b, uint8_t d, uint8_t f, uint8_t off, uint32_t v) {
    io_outl(0xCF8, 0x80000000u | ((uint32_t)b << 16) | ((uint32_t)d << 11) |
                   ((uint32_t)f << 8) | (off & 0xFC));
    io_outl(0xCFC, v);
}

/* enable I/O + memory decoding and bus mastering (DMA) */
static void pci_enable(uint8_t b, uint8_t d, uint8_t f) {
    uint32_t cmd = pci_rd(b, d, f, 0x04);
    pci_wr(b, d, f, 0x04, (cmd & 0xFFFFu) | 0x07);
}

/* ------------------------------------------------------------- e1000 */

#define E_CTRL   0x0000
#define E_STATUS 0x0008
#define E_IMC    0x00D8
#define E_ICR    0x00C0
#define E_RCTL   0x0100
#define E_TCTL   0x0400
#define E_TIPG   0x0410
#define E_RDBAL  0x2800
#define E_RDBAH  0x2804
#define E_RDLEN  0x2808
#define E_RDH    0x2810
#define E_RDT    0x2818
#define E_TDBAL  0x3800
#define E_TDBAH  0x3804
#define E_TDLEN  0x3808
#define E_TDH    0x3810
#define E_TDT    0x3818
#define E_MTA    0x5200
#define E_RAL0   0x5400
#define E_RAH0   0x5404

#define E_RX_N 32
#define E_TX_N 8

struct e_rxd { uint64_t addr; uint16_t length; uint16_t csum; uint8_t status; uint8_t errors; uint16_t special; } __attribute__((packed));
struct e_txd { uint64_t addr; uint16_t length; uint8_t cso; uint8_t cmd; uint8_t status; uint8_t css; uint16_t special; } __attribute__((packed));

static volatile uint8_t* e_mmio;
static struct e_rxd e_rx[E_RX_N] __attribute__((aligned(128)));
static struct e_txd e_tx[E_TX_N] __attribute__((aligned(128)));
static uint8_t e_rxbuf[E_RX_N][2048] __attribute__((aligned(16)));
static uint8_t e_txbuf[E_TX_N][2048] __attribute__((aligned(16)));
static uint32_t e_rx_cur, e_tx_cur;

static inline uint32_t er(uint32_t r) { return *(volatile uint32_t*)(e_mmio + r); }
static inline void ew(uint32_t r, uint32_t v) { *(volatile uint32_t*)(e_mmio + r) = v; }

static int e_link(void) { return (er(E_STATUS) & 2) != 0; }

static int e_send(const void* frame, uint32_t len) {
    if (len > 1514) return -1;
    struct e_txd* d = &e_tx[e_tx_cur];
    uint32_t t0 = get_uptime_ms();
    while (!(d->status & 1)) {                 /* wait for descriptor DD */
        if (get_uptime_ms() - t0 > 100) return -1;
    }
    memcpy(e_txbuf[e_tx_cur], frame, len);
    if (len < 60) { memset(e_txbuf[e_tx_cur] + len, 0, 60 - len); len = 60; }
    d->addr = (uint64_t)(uint32_t)e_txbuf[e_tx_cur];
    d->length = (uint16_t)len;
    d->cmd = 0x0B;                             /* EOP | IFCS | RS */
    d->status = 0;
    e_tx_cur = (e_tx_cur + 1) % E_TX_N;
    asm volatile ("" ::: "memory");
    ew(E_TDT, e_tx_cur);
    return 0;
}

static int e_recv(void* buf, uint32_t cap) {
    for (;;) {
        struct e_rxd* d = &e_rx[e_rx_cur];
        if (!(d->status & 1)) return 0;        /* nothing (DD clear) */
        uint32_t len = d->length;
        int good = (d->status & 2) && d->errors == 0 && len > 0 && len <= cap;
        if (good) memcpy(buf, e_rxbuf[e_rx_cur], len);
        d->status = 0;
        asm volatile ("" ::: "memory");
        ew(E_RDT, e_rx_cur);                   /* hand it back */
        e_rx_cur = (e_rx_cur + 1) % E_RX_N;
        if (good) return (int)len;
    }
}

static int e1000_ids[] = {
    0x1000,0x1001,0x1004,0x1008,0x1009,0x100C,0x100D,0x100E,0x100F,0x1010,
    0x1011,0x1012,0x1013,0x1014,0x1015,0x1016,0x1017,0x1018,0x1019,0x101A,
    0x101D,0x101E,0x1026,0x1027,0x1028,0x1075,0x1076,0x1077,0x1078,0x1079,
    0x107A,0x107B,0x107C,0x10B5,0x10B9,0x10D3,0x1502,0x1503, 0
};

static int e1000_init(uint8_t b, uint8_t d, uint8_t f, nic_t* out) {
    uint32_t bar0 = pci_rd(b, d, f, 0x10);
    if (bar0 & 1) return -1;                   /* I/O BAR: not the MMIO layout */
    pci_enable(b, d, f);
    e_mmio = (volatile uint8_t*)(bar0 & ~0xFu);

    ew(E_IMC, 0xFFFFFFFFu);
    ew(E_CTRL, er(E_CTRL) | (1u << 26));       /* RST */
    timer_delay_ms(20);
    ew(E_IMC, 0xFFFFFFFFu);
    (void)er(E_ICR);
    /* link up, auto speed detect; clear link-reset / PHY-reset / VLAN */
    ew(E_CTRL, (er(E_CTRL) | (1u << 6) | (1u << 5)) &
               ~((1u << 3) | (1u << 31) | (1u << 7) | (1u << 30)));

    uint32_t ral = er(E_RAL0), rah = er(E_RAH0);
    if (ral == 0 && (rah & 0xFFFF) == 0) return -1;
    out->mac[0] = ral;        out->mac[1] = ral >> 8;
    out->mac[2] = ral >> 16;  out->mac[3] = ral >> 24;
    out->mac[4] = rah;        out->mac[5] = rah >> 8;

    int i;
    for (i = 0; i < 128; i++) ew(E_MTA + i * 4, 0);

    for (i = 0; i < E_RX_N; i++) {
        e_rx[i].addr = (uint64_t)(uint32_t)e_rxbuf[i];
        e_rx[i].status = 0;
    }
    ew(E_RDBAL, (uint32_t)e_rx);
    ew(E_RDBAH, 0);
    ew(E_RDLEN, sizeof(e_rx));
    ew(E_RDH, 0);
    e_rx_cur = 0;
    ew(E_RCTL, (1u << 1) | (1u << 15) | (1u << 26));   /* EN | BAM | SECRC, 2K bufs */
    ew(E_RDT, E_RX_N - 1);                             /* tail last: receiving starts here */

    for (i = 0; i < E_TX_N; i++) { e_tx[i].status = 1; e_tx[i].cmd = 0; }
    ew(E_TDBAL, (uint32_t)e_tx);
    ew(E_TDBAH, 0);
    ew(E_TDLEN, sizeof(e_tx));
    ew(E_TDH, 0);
    ew(E_TDT, 0);
    e_tx_cur = 0;
    ew(E_TIPG, 0x0060200A);
    ew(E_TCTL, (1u << 1) | (1u << 3) | (0x0Fu << 4) | (0x40u << 12));  /* EN|PSP|CT|COLD */

    out->name = "e1000";
    out->send = e_send;
    out->recv = e_recv;
    out->link = e_link;
    return 0;
}

/* ----------------------------------------------------------- rtl8139 */

#define R_IDR0  0x00
#define R_TSD0  0x10
#define R_TSAD0 0x20
#define R_RBSTART 0x30
#define R_CR    0x37
#define R_CAPR  0x38
#define R_IMR   0x3C
#define R_TCR   0x40
#define R_RCR   0x44
#define R_CONFIG1 0x52
#define R_BMSR  0x64

#define R_RXLEN 8192
static uint8_t r_rx[R_RXLEN + 16 + 2048] __attribute__((aligned(16)));
static uint8_t r_txbuf[4][2048] __attribute__((aligned(16)));
static uint16_t r_io;
static uint32_t r_rx_off, r_tx_cur;

static int r_link(void) { return (io_inw(r_io + R_BMSR) & 0x0004) != 0; }

static int r_send(const void* frame, uint32_t len) {
    if (len > 1514) return -1;
    uint32_t i = r_tx_cur;
    uint32_t t0 = get_uptime_ms();
    while (!(io_inl(r_io + R_TSD0 + i * 4) & (1u << 13))) {   /* OWN */
        if (get_uptime_ms() - t0 > 100) return -1;
    }
    memcpy(r_txbuf[i], frame, len);
    if (len < 60) { memset(r_txbuf[i] + len, 0, 60 - len); len = 60; }
    io_outl(r_io + R_TSAD0 + i * 4, (uint32_t)r_txbuf[i]);
    io_outl(r_io + R_TSD0 + i * 4, len);                      /* clears OWN -> go */
    r_tx_cur = (i + 1) & 3;
    return 0;
}

static int r_recv(void* buf, uint32_t cap) {
    for (;;) {
        if (io_inb(r_io + R_CR) & 1) return 0;                /* BUFE: empty */
        uint8_t* p = r_rx + r_rx_off;
        uint16_t status = p[0] | (p[1] << 8);
        uint16_t len = p[2] | (p[3] << 8);                    /* includes CRC */
        if (!(status & 1) || len < 18 || len > 1600) {        /* bad ring: resync */
            io_outb(r_io + R_CR, 0x04);                       /* RE off... */
            io_outb(r_io + R_CR, 0x0C);                       /* ...and back on */
            r_rx_off = 0;
            io_outw(r_io + R_CAPR, (uint16_t)(0 - 16));
            return 0;
        }
        uint32_t flen = len - 4;
        int good = flen <= cap;
        if (good) memcpy(buf, p + 4, flen);
        r_rx_off = (r_rx_off + len + 4 + 3) & ~3u;
        io_outw(r_io + R_CAPR, (uint16_t)(r_rx_off - 16));
        r_rx_off %= R_RXLEN;
        if (good) return (int)flen;
    }
}

static int rtl8139_init(uint8_t b, uint8_t d, uint8_t f, nic_t* out) {
    uint32_t bar0 = pci_rd(b, d, f, 0x10);
    if (!(bar0 & 1)) return -1;
    pci_enable(b, d, f);
    r_io = (uint16_t)(bar0 & ~3u);

    io_outb(r_io + R_CONFIG1, 0x00);                          /* power on */
    io_outb(r_io + R_CR, 0x10);                               /* reset */
    uint32_t t0 = get_uptime_ms();
    while (io_inb(r_io + R_CR) & 0x10)
        if (get_uptime_ms() - t0 > 100) return -1;

    int i;
    for (i = 0; i < 6; i++) out->mac[i] = io_inb(r_io + R_IDR0 + i);

    memset(r_rx, 0, sizeof(r_rx));
    io_outl(r_io + R_RBSTART, (uint32_t)r_rx);
    io_outw(r_io + R_IMR, 0x0000);
    io_outl(r_io + R_RCR, 0x8F);              /* AAP... AB|AM|APM|AAP + WRAP, 8K ring */
    io_outb(r_io + R_CR, 0x0C);               /* RE | TE */
    io_outl(r_io + R_TCR, (3u << 24) | (7u << 8));
    r_rx_off = 0;
    r_tx_cur = 0;

    out->name = "rtl8139";
    out->send = r_send;
    out->recv = r_recv;
    out->link = r_link;
    return 0;
}

/* ------------------------------------------------------------- probe */

int nic_probe_pci(nic_t* out) {
    int bus, dev, fn;
    for (bus = 0; bus < 256; bus++) {
        for (dev = 0; dev < 32; dev++) {
            uint32_t id0 = pci_rd(bus, dev, 0, 0x00);
            if ((id0 & 0xFFFF) == 0xFFFF) continue;
            uint32_t hdr = pci_rd(bus, dev, 0, 0x0C);
            int nfn = (hdr & 0x00800000) ? 8 : 1;
            for (fn = 0; fn < nfn; fn++) {
                uint32_t id = pci_rd(bus, dev, fn, 0x00);
                uint16_t ven = id & 0xFFFF, devid = id >> 16;
                if (ven == 0xFFFF) continue;
                if (ven == 0x8086) {
                    int k;
                    for (k = 0; e1000_ids[k]; k++)
                        if (e1000_ids[k] == devid && e1000_init(bus, dev, fn, out) == 0)
                            return 0;
                } else if (ven == 0x10EC && devid == 0x8139) {
                    if (rtl8139_init(bus, dev, fn, out) == 0) return 0;
                }
            }
        }
    }
    return -1;
}
