/* nic.c - PCI NIC drivers for TanjaOS (polled, no interrupts).
 *
 * Supported: Intel PRO/1000 and e1000e (legacy descriptors),
 * Realtek RTL8139, and Realtek RTL8168/8169/8111 (most real hardware). Everything runs in
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

/* PCI */

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

/* Walk up to the host bridge and enable memory + bus master on every
 * bridge. Real machines hide the NIC behind a PCI-PCI bridge; without
 * this, MMIO and DMA never reach the card. */
static void pci_enable_path(uint8_t bus) {
    int b, d, f;
    for (b = 0; b < 256; b++) {
        for (d = 0; d < 32; d++) {
            uint32_t id0 = pci_rd(b, d, 0, 0x00);
            if ((id0 & 0xFFFF) == 0xFFFF) continue;
            int nfn = (pci_rd(b, d, 0, 0x0C) & 0x00800000) ? 8 : 1;
            for (f = 0; f < nfn; f++) {
                uint32_t cls = pci_rd(b, d, f, 0x08);
                if ((cls >> 24) != 0x06 || ((cls >> 16) & 0xFF) != 0x04) continue;
                uint32_t buses = pci_rd(b, d, f, 0x18);
                uint8_t sec = (buses >> 8) & 0xFF, sub = (buses >> 16) & 0xFF;
                if (bus >= sec && bus <= sub)
                    pci_enable(b, d, f);
            }
        }
    }
}

/* If the BIOS left a BAR at 0, assign a safe MMIO window.
 * Handles 64-bit memory BARs (type 0b10 in bits 2:1): the upper half is
 * honoured when the window is addressable, and ignored when the firmware
 * parked it above 4 GiB (this kernel has no PAE, so it cannot reach it). */
static uint32_t pci_bar_mmio(uint8_t b, uint8_t d, uint8_t f, uint8_t off) {
    uint32_t bar = pci_rd(b, d, f, off);
    int is64 = ((bar & 0x6) == 0x4);
    if (bar & 1) return 0;                 /* I/O BAR */
    if (is64) {
        uint32_t hi = pci_rd(b, d, f, (uint8_t)(off + 4));
        if (hi != 0) {
            if ((bar & ~0xFu) != 0) return 0;      /* mapped above 4 GiB: unusable */
            /* unassigned 64-bit BAR: fall through and assign below 4 GiB */
        }
    }
    if ((bar & ~0xFu) != 0) return bar & ~0xFu;
    pci_wr(b, d, f, off, 0xFFFFFFFFu);
    if (is64) pci_wr(b, d, f, (uint8_t)(off + 4), 0xFFFFFFFFu);
    uint32_t sz = pci_rd(b, d, f, off);
    pci_wr(b, d, f, off, 0);
    if (is64) pci_wr(b, d, f, (uint8_t)(off + 4), 0);
    if (sz == 0 || sz == 0xFFFFFFFFu) return 0;
    uint32_t size = (~(sz & ~0xFu)) + 1;
    if (size < 4096) size = 4096;
    /* 0xFE000000 is below the usual APIC/HPET area and above typical RAM. */
    static uint32_t next = 0xFE000000u;
    uint32_t addr = (next + size - 1) & ~(size - 1);
    if (addr + size > 0xFE800000u) return 0;
    pci_wr(b, d, f, off, addr);
    if (is64) pci_wr(b, d, f, (uint8_t)(off + 4), 0);
    next = addr + size;
    return addr;
}

/* e1000 */

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
    0x107A,0x107B,0x107C,0x10B5,0x10B9,0x10D3,0x1502,0x1503,
    /* common real-hardware e1000e: 82574, 82579, I217, I218, I219 */
    0x10D3,0x1502,0x1503,0x153A,0x153B,0x1559,0x155A,0x15A0,0x15A1,
    0x15A2,0x15A3,0x15B7,0x15B8,0x15BB,0x15BC,0x15D7,0x15D8,0x15E3,0x1570,
    0x156F,0x15B9,0x5502,0x5503, 0
};

static int e1000_init(uint8_t b, uint8_t d, uint8_t f, nic_t* out) {
    pci_enable_path(b);
    pci_enable(b, d, f);
    uint32_t bar0 = pci_bar_mmio(b, d, f, 0x10);
    if (!bar0) return -1;
    e_mmio = (volatile uint8_t*)bar0;

    ew(E_IMC, 0xFFFFFFFFu);
    ew(E_CTRL, er(E_CTRL) | (1u << 26));       /* RST */
    {
        uint32_t t0 = get_uptime_ms();
        while (er(E_CTRL) & (1u << 26))
            if (get_uptime_ms() - t0 > 50) break;   /* real silicon can be slow */
    }
    timer_delay_ms(10);
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

/* rtl8139 */

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
/* Real RTL8139 silicon wants the Rx ring not to cross a 64 KiB boundary, so
 * align it to 32 KiB (a multiple of the ring size that cannot straddle one). */
static uint8_t r_rx[R_RXLEN + 16 + 2048] __attribute__((aligned(32768)));
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
    pci_enable_path(b);
    pci_enable(b, d, f);
    uint32_t bar0 = pci_rd(b, d, f, 0x10);
    if (!(bar0 & 1) || !(bar0 & ~3u)) return -1;
    r_io = (uint16_t)(bar0 & ~3u);

    /* 0x50 = Cfg9346: on real chips the CONFIG registers below are
 * write-protected until it is written with 0xC0. */
    io_outb(r_io + 0x50, 0xC0);
    io_outb(r_io + R_CONFIG1, 0x00);                          /* power on, no wake events */
    io_outb(r_io + 0x59, 0x00);                               /* CONFIG3: no cardbus wake */
    io_outb(r_io + 0x5A, 0x80);                               /* CONFIG4: dblfifo (8K ring safe) */
    io_outb(r_io + 0x50, 0x00);                               /* lock */
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


/* rtl8168/8169/8111 */
/* The NIC on most real motherboards and cheap PCI-E cards. Polled,
 * legacy descriptors, no firmware. Covers 8169, 8168, 8111, 8101. */

#define R8_MAC   0x00
#define R8_MAR   0x08
#define R8_TNPDS 0x20
#define R8_CR    0x37
#define R8_TPOLL 0x38
#define R8_IMR   0x3C
#define R8_ISR   0x3E
#define R8_TCR   0x40
#define R8_RCR   0x44
#define R8_9346  0x50
#define R8_CFG1  0x52
#define R8_PHYST 0x6C
#define R8_RMS   0xDA
#define R8_CPCR  0xE0
#define R8_RDSAR 0xE4
#define R8_MTPS  0xEC

#define R8_N 16
struct r8_desc { uint32_t opts1, opts2; uint64_t addr; } __attribute__((packed, aligned(16)));
static struct r8_desc r8_rx[R8_N] __attribute__((aligned(256)));
static struct r8_desc r8_tx[R8_N] __attribute__((aligned(256)));
static uint8_t r8_rxbuf[R8_N][2048] __attribute__((aligned(16)));
static uint8_t r8_txbuf[R8_N][2048] __attribute__((aligned(16)));
static volatile uint8_t* r8_mmio;
static int r8_rx_cur, r8_tx_cur;

static inline uint8_t  r8b(uint32_t o) { return r8_mmio[o]; }
static inline uint16_t r8w(uint32_t o) { return *(volatile uint16_t*)(r8_mmio + o); }
static inline uint32_t r8l(uint32_t o) { return *(volatile uint32_t*)(r8_mmio + o); }
static inline void r8wb(uint32_t o, uint8_t v)  { r8_mmio[o] = v; }
static inline void r8ww(uint32_t o, uint16_t v) { *(volatile uint16_t*)(r8_mmio + o) = v; }
static inline void r8wl(uint32_t o, uint32_t v) { *(volatile uint32_t*)(r8_mmio + o) = v; }

static int r8_link(void) { return (r8b(R8_PHYST) & 0x02) != 0; }

static int r8_send(const void* frame, uint32_t len) {
    if (len > 1514) return -1;
    int i = r8_tx_cur;
    uint32_t t0 = get_uptime_ms();
    while (r8_tx[i].opts1 & 0x80000000u) {
        if (get_uptime_ms() - t0 > 200) return -1;
    }
    if (len < 60) {
        memcpy(r8_txbuf[i], frame, len);
        memset(r8_txbuf[i] + len, 0, 60 - len);
        len = 60;
    } else {
        memcpy(r8_txbuf[i], frame, len);
    }
    r8_tx[i].addr = (uint64_t)(uint32_t)r8_txbuf[i];
    r8_tx[i].opts2 = 0;
    asm volatile ("" ::: "memory");
    r8_tx[i].opts1 = 0x80000000u | 0x20000000u | 0x10000000u | (len & 0x3FFFu); /* OWN|FS|LS */
    asm volatile ("" ::: "memory");
    r8wb(R8_TPOLL, 0x40);                 /* kick normal-priority TX */
    r8_tx_cur = (i + 1) % R8_N;
    return 0;
}

static int r8_recv(void* buf, uint32_t cap) {
    int i = r8_rx_cur;
    if (r8_rx[i].opts1 & 0x80000000u) return 0;   /* still owned by NIC */
    uint32_t opts = r8_rx[i].opts1;
    uint32_t len = opts & 0x3FFFu;
    int good = !(opts & 0x00C00000u) && len >= 14 && len <= cap; /* no RES/RUNT/CRC */
    if (good) memcpy(buf, r8_rxbuf[i], len);
    r8_rx[i].addr = (uint64_t)(uint32_t)r8_rxbuf[i];
    r8_rx[i].opts2 = 0;
    asm volatile ("" ::: "memory");
    r8_rx[i].opts1 = 0x80000000u | (i == R8_N - 1 ? 0x40000000u : 0) | 2048u;
    asm volatile ("" ::: "memory");
    r8_rx_cur = (i + 1) % R8_N;
    return good ? (int)len : 0;
}

static int rtl8169_init(uint8_t b, uint8_t d, uint8_t f, nic_t* out) {
    pci_enable_path(b);
    pci_enable(b, d, f);
    uint32_t mmio = pci_bar_mmio(b, d, f, 0x18);   /* 8168: MMIO is often BAR2 */
    if (!mmio) mmio = pci_bar_mmio(b, d, f, 0x10);
    if (!mmio) return -1;
    r8_mmio = (volatile uint8_t*)mmio;

    r8wb(R8_9346, 0xC0);
    r8wb(R8_CR, 0x10);                                     /* reset */
    {
        uint32_t t0 = get_uptime_ms();
        while (r8b(R8_CR) & 0x10)
            if (get_uptime_ms() - t0 > 200) return -1;
    }
    timer_delay_ms(10);
    r8ww(R8_IMR, 0);                                       /* polled */
    r8ww(R8_ISR, 0xFFFF);
    r8wb(R8_9346, 0xC0);                                   /* unlock config */

    int i;
    for (i = 0; i < 6; i++) out->mac[i] = r8b(R8_MAC + i);
    if ((out->mac[0] | out->mac[1] | out->mac[2] |
         out->mac[3] | out->mac[4] | out->mac[5]) == 0) {
        r8wb(R8_9346, 0);
        return -1;
    }

    for (i = 0; i < R8_N; i++) {
        r8_rx[i].addr = (uint64_t)(uint32_t)r8_rxbuf[i];
        r8_rx[i].opts2 = 0;
        r8_rx[i].opts1 = 0x80000000u | (i == R8_N - 1 ? 0x40000000u : 0) | 2048u;
        r8_tx[i].opts1 = 0;
        r8_tx[i].opts2 = 0;
        r8_tx[i].addr = 0;
    }
    r8_rx_cur = r8_tx_cur = 0;
    r8wl(R8_RDSAR, (uint32_t)r8_rx);
    r8wl(R8_RDSAR + 4, 0);
    r8wl(R8_TNPDS, (uint32_t)r8_tx);
    r8wl(R8_TNPDS + 4, 0);
    r8ww(R8_RMS, 2048);
    r8wb(R8_MTPS, 0x3B);                                   /* max TX packet */
    r8wl(R8_TCR, 0x03000700u);                             /* IFG, MXDMA unlimited */
    r8wl(R8_RCR, 0x0000E70Fu);                             /* unlimited DMA, AB+AM+APM+AAP */
    r8ww(R8_CPCR, (uint16_t)(r8w(R8_CPCR) | 1));           /* checksum offload off is fine */
    r8wb(R8_CR, 0x0C);                                     /* TE | RE */
    r8wb(R8_9346, 0x00);                                   /* lock */
    r8wb(R8_TPOLL, 0xFF);

    out->name = "rtl8169";
    out->send = r8_send;
    out->recv = r8_recv;
    out->link = r8_link;
    return 0;
}


/* Log every PCI network function so a failed probe still says what the
 * machine actually has. */
extern void boot_log(const char* s);
extern void print_hex(uint32_t n);

/* ne2000 / RTL8029AS */
/* Novell NE2000 and its clones: Realtek RTL8029AS (10EC:8029), the card
 * QEMU calls "ne2k_pci", and the ISA cards of 486 / Pentium class machines.
 * dp8390 register set, 16 KiB of card RAM: TX page 0x40, Rx ring 0x46..0x7F.
 * Polled, 16-bit remote DMA, no interrupts. The chip has no link register,
 * so link() reports up and DHCP tells the truth about the cable. */

#define N_CR      0x00
#define N_PSTART  0x01
#define N_PSTOP   0x02
#define N_BNRY    0x03
#define N_TPSR    0x04
#define N_TSR     0x04      /* same offset, read side: transmit status */
#define N_TBCR0   0x05
#define N_TBCR1   0x06
#define N_ISR     0x07
#define N_RSAR0   0x08
#define N_RSAR1   0x09
#define N_RBCR0   0x0A
#define N_RBCR1   0x0B
#define N_RCR     0x0C
#define N_TCR     0x0D
#define N_DCR     0x0E
#define N_IMR     0x0F
#define N_DATA    0x10
#define N_RESET   0x1F

#define N_TX_PAGE  0x40
#define N_RX_START 0x46
#define N_RX_STOP  0x80

static uint16_t n_io;
static int n_tx_pending;

/* start a remote DMA transfer; cr is STA|RDMA|RREAD (0x32) or ...|RWRITE (0x2A) */
static void n_dma_setup(uint8_t cr, uint32_t addr, uint32_t len) {
    io_outb(n_io + N_ISR, 0x40);                       /* clear the DMA counter */
    io_outb(n_io + N_RBCR0, (uint8_t)(len & 0xFF));
    io_outb(n_io + N_RBCR1, (uint8_t)((len >> 8) & 0xFF));
    io_outb(n_io + N_RSAR0, (uint8_t)(addr & 0xFF));
    io_outb(n_io + N_RSAR1, (uint8_t)((addr >> 8) & 0xFF));
    io_outb(n_io + N_CR, cr);
}

static void n_dma_done(void) {
    uint32_t t0 = get_uptime_ms();
    while ((io_inb(n_io + N_RBCR0) || io_inb(n_io + N_RBCR1)) &&
           get_uptime_ms() - t0 < 20) { }
    io_outb(n_io + N_CR, 0x22);                        /* page 0, started, no DMA */
}

static void n_mem_read(uint32_t addr, uint8_t* dst, uint32_t len) {
    uint32_t i;
    n_dma_setup(0x32, addr, len);                      /* STA | RDMA | RREAD */
    for (i = 0; i + 1 < len; i += 2) {
        uint16_t w = io_inw(n_io + N_DATA);
        dst[i] = (uint8_t)w;
        dst[i + 1] = (uint8_t)(w >> 8);
    }
    if (len & 1) dst[len - 1] = io_inb(n_io + N_DATA);
    n_dma_done();
}

static void n_mem_write(uint32_t addr, const uint8_t* src, uint32_t len) {
    uint32_t i;
    n_dma_setup(0x2A, addr, len);                      /* STA | RDMA | RWRITE */
    for (i = 0; i + 1 < len; i += 2)
        io_outw(n_io + N_DATA, (uint16_t)src[i] | ((uint16_t)src[i + 1] << 8));
    if (len & 1) io_outb(n_io + N_DATA, src[len - 1]);
    n_dma_done();
}

static int n_link(void) { return 1; }   /* no link register on this chip */

static int n_send(const void* frame, uint32_t len) {
    static uint8_t pad[64];
    uint32_t t0;
    if (len > 1514) return -1;
    if (n_tx_pending) {                                /* previous frame still going */
        /* TSR bit 0 = PTX. ISR cannot be used here: the receive path clears
 * ISR to acknowledge packets, which would take PTX with it. */
        t0 = get_uptime_ms();
        while (!(io_inb(n_io + N_TSR) & 0x01)) {
            if (get_uptime_ms() - t0 > 200) return -1;
        }
        io_outb(n_io + N_TSR, 0xFF);
        n_tx_pending = 0;
    }
    if (len < 60) {
        memcpy(pad, frame, len);
        memset(pad + len, 0, 60 - len);
        frame = pad;
        len = 60;
    }
    n_mem_write((uint32_t)N_TX_PAGE << 8, (const uint8_t*)frame, len);
    io_outb(n_io + N_TPSR, N_TX_PAGE);
    io_outb(n_io + N_TBCR0, (uint8_t)(len & 0xFF));
    io_outb(n_io + N_TBCR1, (uint8_t)((len >> 8) & 0xFF));
    io_outb(n_io + N_CR, 0x26);                        /* STA | TXP */
    n_tx_pending = 1;
    return 0;
}

static int n_recv(void* buf, uint32_t cap) {
    /* Keep pulling frames while they are queued, dropping any that do not fit
 * in the caller's buffer: the 14 KiB card ring overflows if the stack
 * spends time on disk I/O between polls. */
    int guard;
    for (guard = 0; guard < 8; guard++) {
        uint8_t hdr[4], bnry, next, page;
        uint16_t size;
        uint32_t flen;
        if (!(io_inb(n_io + N_ISR) & 0x01)) return 0;  /* nothing received */
        bnry = io_inb(n_io + N_BNRY);
        page = (bnry == N_RX_STOP - 1) ? N_RX_START : (uint8_t)(bnry + 1);
        n_mem_read((uint32_t)page << 8, hdr, 4);       /* status, next page, length */
        next = hdr[1];
        size = (uint16_t)hdr[2] | ((uint16_t)hdr[3] << 8);
        if (!(hdr[0] & 1) || size < 4 || size > 1526 ||
            next < N_RX_START || next > N_RX_STOP || next == page) {
            /* partial or oversize frame: the ring is out of step, rebuild it */
            io_outb(n_io + N_BNRY, N_RX_START - 1);            /* boundary = last page consumed */
            io_outb(n_io + N_ISR, 0xFF);
            return 0;
        }
        flen = (uint32_t)size - 4;
        if (flen <= cap) n_mem_read(((uint32_t)page << 8) + 4, (uint8_t*)buf, flen);
        /* boundary = last page of this packet, wrapping back to PSTOP-1 */
        io_outb(n_io + N_BNRY, (uint8_t)(next == N_RX_START ? N_RX_STOP - 1 : next - 1));
        io_outb(n_io + N_ISR, 0xFF);
        if (flen <= cap && flen >= 14) return (int)flen;
    }
    return 0;
}

static int ne2k_init(uint8_t b, uint8_t d, uint8_t f, nic_t* out) {
    uint32_t bar0;
    int i;
    pci_enable_path(b);
    pci_enable(b, d, f);
    bar0 = pci_rd(b, d, f, 0x10);
    if (!(bar0 & 1) || !(bar0 & ~3u)) return -1;
    n_io = (uint16_t)(bar0 & ~3u);

    io_outb(n_io + N_IMR, 0x00);                       /* polled: no interrupts */
    io_outb(n_io + N_ISR, 0xFF);
    (void)io_inb(n_io + N_RESET);                      /* the classic reset: */
    io_outb(n_io + N_RESET, 0xFF);                     /* read, write, wait */
    timer_delay_ms(5);
    io_outb(n_io + N_CR, 0x21);                        /* page 0, stopped, no DMA */
    io_outb(n_io + N_DCR, 0x49);                       /* 16-bit transfers */
    io_outb(n_io + N_RBCR0, 0);
    io_outb(n_io + N_RBCR1, 0);
    io_outb(n_io + N_RCR, 0x20);                       /* monitor while setting up */
    io_outb(n_io + N_TCR, 0x02);                       /* on-chip loopback */
    io_outb(n_io + N_TPSR, N_TX_PAGE);
    io_outb(n_io + N_PSTART, N_RX_START);
    io_outb(n_io + N_PSTOP, N_RX_STOP);
    io_outb(n_io + N_BNRY, N_RX_START - 1);            /* boundary = last page consumed */

    /* MAC: real cards load PAR from their PROM at power-up; emulated ones
 * leave it zero, so fall back to reading the PROM through remote DMA. */
    io_outb(n_io + N_CR, 0x61);                        /* page 1 */
    for (i = 0; i < 6; i++) out->mac[i] = io_inb(n_io + 0x01 + i);
    if (!(out->mac[0] | out->mac[1] | out->mac[2] |
          out->mac[3] | out->mac[4] | out->mac[5])) {
        uint8_t prom[16];
        n_mem_read(0, prom, 16);
        if (prom[0] == 0x57 && prom[1] == 0x57) {
            for (i = 0; i < 6; i++) out->mac[i] = prom[2 + i];   /* NE2000 PROM: 57 57 then the node id */
        } else {
            /* 16-bit PROM read through a byte-wide port repeats each byte */
            for (i = 0; i < 6; i++) out->mac[i] = prom[i * 2];
        }
        io_outb(n_io + N_CR, 0x61);                    /* n_mem_read left page 0 */
    }
    for (i = 0; i < 6; i++) io_outb(n_io + 0x01 + i, out->mac[i]);   /* PAR */


    io_outb(n_io + 0x07, N_RX_START);                  /* CURR = first free page */
    io_outb(n_io + N_CR, 0x21);                        /* page 0 again */
    io_outb(n_io + N_RCR, 0x0C);                       /* accept broadcast + multicast */
    io_outb(n_io + N_TCR, 0x00);                       /* normal transmit */
    io_outb(n_io + N_ISR, 0xFF);
    io_outb(n_io + N_CR, 0x22);                        /* start */
    n_tx_pending = 0;

    if (!(out->mac[0] | out->mac[1] | out->mac[2] |
          out->mac[3] | out->mac[4] | out->mac[5])) return -1;
    out->name = "ne2000";
    out->send = n_send;
    out->recv = n_recv;
    out->link = n_link;
    return 0;
}

static int ne2k_match(uint16_t ven, uint16_t dev) {
    return ven == 0x10EC && (dev == 0x8029 || dev == 0x8028);
}

/* atheros AR8132 */
/* Attansic L2C / Atheros AR8132 (PCI 1969:1062). Common on ASUS Eee PC.
 * Polled, one TX ring + one RX free ring + one RX return ring.
 * RX buffers must be <= 1536 or the chip locks up. */

#define A_MASTER   0x1400
#define A_MDIO     0x1414
#define A_PHYSTAT  0x1418
#define A_MACCFG   0x1480
#define A_PAR0     0x1488
#define A_PAR1     0x148C
#define A_RXHI     0x1540
#define A_TXHI     0x1544
#define A_RD0      0x1550
#define A_RDCNT    0x1560
#define A_RXBUFSZ  0x1564
#define A_RRD0     0x1568
#define A_RRDCNT   0x1578
#define A_TDL      0x1580
#define A_TDCNT    0x1584
#define A_M_RDPROD 0x15E0
#define A_M_TDPROD 0x15F0
#define A_M_TDCONS 0x15F4

#define A_N 16
#define A_BUFSZ 1536

struct a_rxd { uint64_t addr; } __attribute__((packed));
struct a_rrd { uint32_t info, rss, vtag, status; } __attribute__((packed));
struct a_txd { uint32_t len, flags; uint64_t addr; } __attribute__((packed));

static struct a_rxd a_rx[A_N] __attribute__((aligned(16)));
static struct a_rrd a_rr[A_N] __attribute__((aligned(16)));
static struct a_txd a_tx[A_N] __attribute__((aligned(16)));
static uint8_t a_rxbuf[A_N][A_BUFSZ] __attribute__((aligned(16)));
static uint8_t a_txbuf[A_N][A_BUFSZ] __attribute__((aligned(16)));
static volatile uint8_t* a_mmio;
static int a_rx_cur, a_tx_cur;

static inline uint32_t ar(uint32_t o) { return *(volatile uint32_t*)(a_mmio + o); }
static inline void aw(uint32_t o, uint32_t v) { *(volatile uint32_t*)(a_mmio + o) = v; }

static int a_link(void) { return (ar(A_PHYSTAT) & 1) != 0; }

static int a_send(const void* frame, uint32_t len) {
    if (len > 1514) return -1;
    int i = a_tx_cur;
    uint32_t t0 = get_uptime_ms();
    /* wait until the consumer has passed this slot */
    while (((ar(A_M_TDCONS) & 0xFFFF) % A_N) == (uint32_t)((i + 1) % A_N)) {
        if (get_uptime_ms() - t0 > 200) return -1;
    }
    if (len < 60) {
        memcpy(a_txbuf[i], frame, len);
        memset(a_txbuf[i] + len, 0, 60 - len);
        len = 60;
    } else {
        memcpy(a_txbuf[i], frame, len);
    }
    a_tx[i].addr = (uint64_t)(uint32_t)a_txbuf[i];
    a_tx[i].len = len & 0x3FFF;
    a_tx[i].flags = 0x80000000u;          /* EOP */
    asm volatile ("" ::: "memory");
    a_tx_cur = (i + 1) % A_N;
    aw(A_M_TDPROD, (uint32_t)a_tx_cur);
    return 0;
}

static int a_recv(void* buf, uint32_t cap) {
    int i = a_rx_cur;
    uint32_t st = a_rr[i].status;
    if (!(st & 0x80000000u)) return 0;    /* not valid */
    uint32_t len = st & 0x3FFF;
    int good = !(st & 0x03F00000u) && len >= 14 && len <= cap;
    if (good) memcpy(buf, a_rxbuf[i], len);
    a_rr[i].status = 0;
    a_rx[i].addr = (uint64_t)(uint32_t)a_rxbuf[i];
    asm volatile ("" ::: "memory");
    a_rx_cur = (i + 1) % A_N;
    aw(A_M_RDPROD, (uint32_t)a_rx_cur);
    return good ? (int)len : 0;
}

static int ar8132_init(uint8_t b, uint8_t d, uint8_t f, nic_t* out) {
    pci_enable_path(b);
    pci_enable(b, d, f);
    uint32_t bar = pci_bar_mmio(b, d, f, 0x10);
    if (!bar) return -1;
    a_mmio = (volatile uint8_t*)bar;

    aw(A_MASTER, ar(A_MASTER) | 1u);       /* soft reset */
    {
        uint32_t t0 = get_uptime_ms();
        while (ar(A_MASTER) & 1u)
            if (get_uptime_ms() - t0 > 100) break;
    }
    timer_delay_ms(10);

    uint32_t lo = ar(A_PAR0), hi = ar(A_PAR1);
    /* PAR0 is mac[2..5] big-endian-ish: low dword = mac2<<24|mac3<<16|mac4<<8|mac5 */
    out->mac[2] = lo >> 24; out->mac[3] = lo >> 16;
    out->mac[4] = lo >> 8;  out->mac[5] = lo;
    out->mac[0] = hi >> 8;  out->mac[1] = hi;
    if ((out->mac[0] | out->mac[1] | out->mac[2] |
         out->mac[3] | out->mac[4] | out->mac[5]) == 0)
        return -1;

    int i;
    for (i = 0; i < A_N; i++) {
        a_rx[i].addr = (uint64_t)(uint32_t)a_rxbuf[i];
        a_rr[i].status = 0;
        a_tx[i].flags = 0;
        a_tx[i].len = 0;
    }
    a_rx_cur = a_tx_cur = 0;

    aw(A_RXHI, 0);
    aw(A_TXHI, 0);
    aw(A_RD0, (uint32_t)a_rx);
    aw(A_RDCNT, A_N);
    aw(A_RXBUFSZ, A_BUFSZ);
    aw(A_RRD0, (uint32_t)a_rr);
    aw(A_RRDCNT, A_N);
    aw(A_TDL, (uint32_t)a_tx);
    aw(A_TDCNT, A_N);
    aw(A_M_RDPROD, 0);
    aw(A_M_TDPROD, 0);

    /* TX|RX enable, full duplex, add CRC, pad, broadcast */
    aw(A_MACCFG, (1u << 0) | (1u << 1) | (1u << 5) | (1u << 6) |
                 (1u << 7) | (1u << 26));
    aw(A_M_RDPROD, A_N - 1);               /* hand RX buffers to the chip */
    out->name = "ar8132";
    out->send = a_send;
    out->recv = a_recv;
    out->link = a_link;
    return 0;
}


/* virtio-net */
/* The NIC of KVM/QEMU, Proxmox, most clouds, VirtualBox and UTM. Both the
 * legacy (I/O port) and the modern (PCI capability / MMIO) transport. Polled. */

#define VQ_N     256                 /* modern queues are shrunk to this; legacy keeps the device size (<=1024) */
#define V_RXBUFS 32
#define V_TXBUFS 16
struct vdesc { uint64_t addr; uint32_t len; uint16_t flags, next; } __attribute__((packed));
struct vq {
    struct vdesc* desc; volatile uint16_t* avail; volatile uint8_t* used;
    uint16_t qs, avail_idx, last_used;
};
static uint8_t v_legacy_mem[2][32768] __attribute__((aligned(4096)));
static struct vdesc v_m_desc[2][VQ_N]       __attribute__((aligned(16)));
static uint16_t     v_m_avail[2][VQ_N + 4]  __attribute__((aligned(4)));
static uint8_t      v_m_used[2][8 + 8 * VQ_N] __attribute__((aligned(4)));
static uint8_t v_rxbuf[V_RXBUFS][2048] __attribute__((aligned(16)));
static uint8_t v_txbuf[V_TXBUFS][2048] __attribute__((aligned(16)));
static struct vq v_rx, v_tx;
static int v_modern, v_hdr, v_has_status;
static uint16_t v_io;                       /* legacy: I/O base */
static volatile uint8_t *v_common, *v_notify, *v_devcfg;
static uint32_t v_notify_mult;
static volatile uint16_t* v_rx_notify; static volatile uint16_t* v_tx_notify;
static uint8_t v_txbusy[V_TXBUFS];

static void v_kick(int q) {
    asm volatile ("mfence" ::: "memory");
    if (v_modern) *(q ? v_tx_notify : v_rx_notify) = q;
    else io_outw(v_io + 0x10, q);
}

static void vq_attach_legacy(struct vq* q, uint8_t* mem, uint16_t qs) {
    q->qs = qs;
    q->desc = (struct vdesc*)mem;
    q->avail = (volatile uint16_t*)(mem + 16u * qs);
    q->used = (volatile uint8_t*)(((uint32_t)(mem + 16u * qs + 6 + 2u * qs) + 4095) & ~4095u);
    q->avail_idx = q->last_used = 0;
}

static void v_post_rx(int i) {
    v_rx.desc[i].addr = (uint64_t)(uint32_t)v_rxbuf[i];
    v_rx.desc[i].len = 2048; v_rx.desc[i].flags = 2; v_rx.desc[i].next = 0;     /* device writes */
    v_rx.avail[2 + (v_rx.avail_idx % v_rx.qs)] = i;
    asm volatile ("" ::: "memory");
    v_rx.avail_idx++;
    v_rx.avail[1] = v_rx.avail_idx;
}

static int v_link(void) {
    if (!v_has_status) return 1;
    return (v_modern ? *(volatile uint16_t*)(v_devcfg + 6) : io_inw(v_io + 0x14 + 6)) & 1;
}

static void v_reclaim(void) {
    while (v_tx.last_used != *(volatile uint16_t*)(v_tx.used + 2)) {
        uint32_t id = *(volatile uint32_t*)(v_tx.used + 4 + 8 * (v_tx.last_used % v_tx.qs));
        if (id < V_TXBUFS) v_txbusy[id] = 0;
        v_tx.last_used++;
    }
}

static int v_send(const void* frame, uint32_t len) {
    int i, slot = -1;
    uint32_t t0 = get_uptime_ms();
    if (len > 1514) return -1;
    for (;;) {
        v_reclaim();
        for (i = 0; i < V_TXBUFS; i++) if (!v_txbusy[i]) { slot = i; break; }
        if (slot >= 0) break;
        if (get_uptime_ms() - t0 > 200) return -1;
    }
    memset(v_txbuf[slot], 0, v_hdr);                       /* virtio_net_hdr: no offloads */
    memcpy(v_txbuf[slot] + v_hdr, frame, len);
    v_tx.desc[slot].addr = (uint64_t)(uint32_t)v_txbuf[slot];
    v_tx.desc[slot].len = v_hdr + len; v_tx.desc[slot].flags = 0; v_tx.desc[slot].next = 0;
    v_txbusy[slot] = 1;
    v_tx.avail[2 + (v_tx.avail_idx % v_tx.qs)] = slot;
    asm volatile ("" ::: "memory");
    v_tx.avail_idx++;
    v_tx.avail[1] = v_tx.avail_idx;
    v_kick(1);
    return 0;
}

static int v_recv(void* buf, uint32_t cap) {
    while (v_rx.last_used != *(volatile uint16_t*)(v_rx.used + 2)) {
        uint32_t k = v_rx.last_used % v_rx.qs;
        uint32_t id = *(volatile uint32_t*)(v_rx.used + 4 + 8 * k);
        uint32_t len = *(volatile uint32_t*)(v_rx.used + 8 + 8 * k);
        int good = id < V_RXBUFS && len > v_hdr + 14 && len - v_hdr <= cap;
        if (good) memcpy(buf, v_rxbuf[id] + v_hdr, len - v_hdr);
        v_rx.last_used++;
        if (id < V_RXBUFS) { v_post_rx(id); v_kick(0); }
        if (good) return (int)(len - v_hdr);
    }
    return 0;
}

static void v_status(uint8_t st) {
    if (v_modern) *(volatile uint8_t*)(v_common + 20) = st; else io_outb(v_io + 0x12, st);
}

static int virtio_legacy_init(uint8_t b, uint8_t d, uint8_t f, nic_t* out) {
    uint32_t bar0 = pci_rd(b, d, f, 0x10);
    int q, i;
    if (!(bar0 & 1)) return -1;
    pci_enable_path(b); pci_enable(b, d, f);
    v_io = (uint16_t)(bar0 & ~3u);
    v_modern = 0; v_hdr = 10;
    io_outb(v_io + 0x12, 0);                                /* reset */
    io_outb(v_io + 0x12, 1);                                /* ACKNOWLEDGE */
    io_outb(v_io + 0x12, 1 | 2);                            /* DRIVER */
    {
        uint32_t feat = io_inl(v_io + 0);
        v_has_status = (feat >> 16) & 1;
        io_outl(v_io + 4, feat & ((1u << 5) | (1u << 16)));    /* MAC, STATUS - nothing else */
    }
    for (q = 0; q < 2; q++) {
        uint16_t qs;
        io_outw(v_io + 0x0E, q);
        qs = io_inw(v_io + 0x0C);
        if (qs == 0 || qs > 1024 || (qs & (qs - 1))) return -1;
        memset(v_legacy_mem[q], 0, sizeof(v_legacy_mem[q]));
        vq_attach_legacy(q ? &v_tx : &v_rx, v_legacy_mem[q], qs);
        io_outl(v_io + 0x08, (uint32_t)v_legacy_mem[q] >> 12);
    }
    for (i = 0; i < 6; i++) out->mac[i] = io_inb(v_io + 0x14 + i);
    for (i = 0; i < V_RXBUFS && i < v_rx.qs; i++) v_post_rx(i);
    memset(v_txbusy, 0, sizeof(v_txbusy));
    io_outb(v_io + 0x12, 1 | 2 | 4);                        /* DRIVER_OK */
    v_kick(0);
    out->name = "virtio-net"; out->send = v_send; out->recv = v_recv; out->link = v_link;
    return 0;
}

static int virtio_modern_init(uint8_t b, uint8_t d, uint8_t f, nic_t* out) {
    uint32_t stat = pci_rd(b, d, f, 0x04) >> 16;
    uint8_t cap;
    int i, q;
    volatile uint8_t *common = 0, *notify = 0, *devcfg = 0;
    uint32_t mult = 0;
    if (!(stat & 0x10)) return -1;                          /* no capability list */
    pci_enable_path(b); pci_enable(b, d, f);
    for (cap = pci_rd(b, d, f, 0x34) & 0xFC; cap; cap = (pci_rd(b, d, f, cap) >> 8) & 0xFC) {
        uint32_t w0 = pci_rd(b, d, f, cap);
        uint8_t type, bar;
        uint32_t off, bar_lo, base;
        if ((w0 & 0xFF) != 0x09) continue;                  /* vendor-specific capability */
        type = (w0 >> 24) & 0xFF;
        bar = pci_rd(b, d, f, cap + 4) & 0xFF;
        off = pci_rd(b, d, f, cap + 8);
        if (bar > 5) continue;
        bar_lo = pci_rd(b, d, f, 0x10 + 4 * bar);
        if (bar_lo & 1) continue;
        if (((bar_lo >> 1) & 3) == 2 && pci_rd(b, d, f, 0x14 + 4 * bar) != 0) continue;   /* 64-bit BAR above 4 GB */
        base = pci_bar_mmio(b, d, f, 0x10 + 4 * bar);
        if (!base) continue;
        if (type == 1) common = (volatile uint8_t*)(base + off);
        else if (type == 2) { notify = (volatile uint8_t*)(base + off); mult = pci_rd(b, d, f, cap + 16); }
        else if (type == 4) devcfg = (volatile uint8_t*)(base + off);
    }
    if (!common || !notify || !devcfg) return -1;
    v_modern = 1; v_hdr = 12; v_common = common; v_notify = notify; v_devcfg = devcfg; v_notify_mult = mult;

    v_status(0);
    { uint32_t t0 = get_uptime_ms(); while (*(volatile uint8_t*)(common + 20) && get_uptime_ms() - t0 < 200) ; }
    v_status(1); v_status(1 | 2);
    *(volatile uint32_t*)(common + 0) = 1;                  /* device features, high word: VERSION_1 is bit 32 */
    if (!(*(volatile uint32_t*)(common + 4) & 1)) return -1;
    *(volatile uint32_t*)(common + 0) = 0;
    v_has_status = (*(volatile uint32_t*)(common + 4) >> 16) & 1;
    *(volatile uint32_t*)(common + 8) = 0;                  /* driver features low: MAC, STATUS */
    *(volatile uint32_t*)(common + 12) = (1u << 5) | (v_has_status ? (1u << 16) : 0);
    *(volatile uint32_t*)(common + 8) = 1;
    *(volatile uint32_t*)(common + 12) = 1;                 /* VERSION_1 */
    v_status(1 | 2 | 8);                                    /* FEATURES_OK */
    if (!(*(volatile uint8_t*)(common + 20) & 8)) return -1;

    for (q = 0; q < 2; q++) {
        struct vq* v = q ? &v_tx : &v_rx;
        uint16_t qs;
        *(volatile uint16_t*)(common + 22) = q;
        qs = *(volatile uint16_t*)(common + 24);
        if (qs == 0) return -1;
        if (qs > VQ_N) qs = VQ_N;
        *(volatile uint16_t*)(common + 24) = qs;
        memset(v_m_desc[q], 0, sizeof(v_m_desc[q])); memset(v_m_avail[q], 0, sizeof(v_m_avail[q])); memset(v_m_used[q], 0, sizeof(v_m_used[q]));
        v->qs = qs; v->desc = v_m_desc[q]; v->avail = v_m_avail[q]; v->used = v_m_used[q]; v->avail_idx = v->last_used = 0;
        *(volatile uint32_t*)(common + 32) = (uint32_t)v_m_desc[q];  *(volatile uint32_t*)(common + 36) = 0;
        *(volatile uint32_t*)(common + 40) = (uint32_t)v_m_avail[q]; *(volatile uint32_t*)(common + 44) = 0;
        *(volatile uint32_t*)(common + 48) = (uint32_t)v_m_used[q];  *(volatile uint32_t*)(common + 52) = 0;
        {
            uint16_t noff = *(volatile uint16_t*)(common + 30);
            volatile uint16_t* n = (volatile uint16_t*)(notify + (uint32_t)noff * mult);
            if (q) v_tx_notify = n; else v_rx_notify = n;
        }
        *(volatile uint16_t*)(common + 28) = 1;             /* queue_enable */
    }
    for (i = 0; i < 6; i++) out->mac[i] = devcfg[i];
    for (i = 0; i < V_RXBUFS && i < v_rx.qs; i++) v_post_rx(i);
    memset(v_txbusy, 0, sizeof(v_txbusy));
    v_status(1 | 2 | 8 | 4);                                /* DRIVER_OK */
    v_kick(0);
    out->name = "virtio-net"; out->send = v_send; out->recv = v_recv; out->link = v_link;
    return 0;
}

static int virtio_init(uint8_t b, uint8_t d, uint8_t f, nic_t* out) {
    uint32_t id = pci_rd(b, d, f, 0x00);
    uint16_t dev = id >> 16;
    if (dev == 0x1000) {                                    /* transitional: prefer legacy I/O unless the BAR isn't I/O */
        if (virtio_legacy_init(b, d, f, out) == 0) return 0;
        return virtio_modern_init(b, d, f, out);
    }
    return virtio_modern_init(b, d, f, out);               /* 0x1041 */
}
static int virtio_match(uint16_t ven, uint16_t dev) { return ven == 0x1AF4 && (dev == 0x1000 || dev == 0x1041); }

/* AMD PCnet */
/* PCnet-PCI II / FAST III: VirtualBox's default NIC, old VMware, QEMU "pcnet". */

#define P_N 16
struct p_desc { uint32_t addr; uint32_t w1; uint32_t w2; uint32_t w3; } __attribute__((packed));
struct p_init { uint16_t mode; uint8_t rlen, tlen; uint8_t padr[6]; uint16_t res; uint8_t ladrf[8]; uint32_t rdra, tdra; } __attribute__((packed));
static struct p_desc p_rx[P_N] __attribute__((aligned(16)));
static struct p_desc p_tx[P_N] __attribute__((aligned(16)));
static struct p_init p_blk __attribute__((aligned(4)));
static uint8_t p_rxbuf[P_N][1536] __attribute__((aligned(16)));
static uint8_t p_txbuf[P_N][1536] __attribute__((aligned(16)));
static uint16_t p_io;
static int p_rx_cur, p_tx_cur;

static uint32_t p_rcsr(int r) { io_outl(p_io + 0x14, r); return io_inl(p_io + 0x10); }
static void     p_wcsr(int r, uint32_t v) { io_outl(p_io + 0x14, r); io_outl(p_io + 0x10, v); }
static void     p_wbcr(int r, uint32_t v) { io_outl(p_io + 0x14, r); io_outl(p_io + 0x1C, v); }

static int p_link(void) { return 1; }                      /* virtual NIC: the cable is always in */

static int p_send(const void* frame, uint32_t len) {
    int i = p_tx_cur;
    uint32_t t0 = get_uptime_ms();
    if (len > 1514) return -1;
    while (p_tx[i].w1 & 0x80000000u) if (get_uptime_ms() - t0 > 200) return -1;
    memcpy(p_txbuf[i], frame, len);
    if (len < 60) { memset(p_txbuf[i] + len, 0, 60 - len); len = 60; }
    p_tx[i].addr = (uint32_t)p_txbuf[i];
    p_tx[i].w2 = 0;
    asm volatile ("" ::: "memory");
    p_tx[i].w1 = 0x80000000u | 0x02000000u | 0x01000000u | 0x0000F000u | ((-len) & 0xFFF);   /* OWN | STP | ENP | -len */
    asm volatile ("" ::: "memory");
    p_wcsr(0, 0x0008);                                      /* TDMD: transmit now */
    p_tx_cur = (i + 1) % P_N;
    return 0;
}

static int p_recv(void* buf, uint32_t cap) {
    int i = p_rx_cur;
    uint32_t w1, len;
    if (p_rx[i].w1 & 0x80000000u) return 0;
    w1 = p_rx[i].w1;
    len = p_rx[i].w2 & 0xFFF;                               /* includes the CRC */
    int good = !(w1 & 0x40000000u) && (w1 & 0x03000000u) == 0x03000000u && len > 4 + 14 && len - 4 <= cap;
    if (good) memcpy(buf, p_rxbuf[i], len - 4);
    p_rx[i].w2 = 0;
    asm volatile ("" ::: "memory");
    p_rx[i].w1 = 0x80000000u | 0x0000F000u | ((-1536) & 0xFFF);
    p_rx_cur = (i + 1) % P_N;
    return good ? (int)(len - 4) : 0;
}

static int pcnet_init(uint8_t b, uint8_t d, uint8_t f, nic_t* out) {
    uint32_t bar0 = pci_rd(b, d, f, 0x10);
    int i;
    if (!(bar0 & 1)) return -1;
    pci_enable_path(b); pci_enable(b, d, f);
    p_io = (uint16_t)(bar0 & ~3u);
    (void)io_inl(p_io + 0x18); (void)io_inw(p_io + 0x14);   /* reset (32-bit and 16-bit mode ports) */
    timer_delay_ms(5);
    for (i = 0; i < 6; i++) out->mac[i] = io_inb(p_io + i); /* address PROM, readable in 16-bit mode */
    io_outl(p_io + 0x10, 0);                                /* a dword write switches the chip to 32-bit I/O */
    p_wbcr(20, 2);                                          /* software style 2: 32-bit descriptors */
    {
        int all0 = 1, allf = 1;
        for (i = 0; i < 6; i++) { if (out->mac[i]) all0 = 0; if (out->mac[i] != 0xFF) allf = 0; }
        if (all0 || allf) {                                  /* PROM unreadable: use the physical address registers */
            uint32_t c12 = p_rcsr(12), c13 = p_rcsr(13), c14 = p_rcsr(14);
            out->mac[0] = c12; out->mac[1] = c12 >> 8; out->mac[2] = c13; out->mac[3] = c13 >> 8;
            out->mac[4] = c14; out->mac[5] = c14 >> 8;
        }
        if ((out->mac[0] | out->mac[1] | out->mac[2] | out->mac[3] | out->mac[4] | out->mac[5]) == 0 ||
            (out->mac[0] & out->mac[1] & out->mac[2] & out->mac[3] & out->mac[4] & out->mac[5]) == 0xFF) return -1;
    }

    for (i = 0; i < P_N; i++) {
        p_rx[i].addr = (uint32_t)p_rxbuf[i]; p_rx[i].w2 = 0; p_rx[i].w3 = 0;
        p_rx[i].w1 = 0x80000000u | 0x0000F000u | ((-1536) & 0xFFF);
        p_tx[i].addr = (uint32_t)p_txbuf[i]; p_tx[i].w1 = 0; p_tx[i].w2 = 0; p_tx[i].w3 = 0;
    }
    p_rx_cur = p_tx_cur = 0;
    memset(&p_blk, 0, sizeof(p_blk));
    p_blk.mode = 0;
    p_blk.rlen = 4 << 4; p_blk.tlen = 4 << 4;               /* 2^4 = 16 descriptors each */
    memcpy(p_blk.padr, out->mac, 6);
    p_blk.rdra = (uint32_t)p_rx; p_blk.tdra = (uint32_t)p_tx;
    memset(p_blk.ladrf, 0xFF, 8);                           /* accept all multicast */
    p_wcsr(1, (uint32_t)&p_blk & 0xFFFF);
    p_wcsr(2, (uint32_t)&p_blk >> 16);
    p_wcsr(3, 0x5F00);                                      /* mask interrupts (polled) */
    p_wcsr(4, 0x0915);                                      /* auto-pad TX, strip RX pad, no TX poll interrupts */
    p_wcsr(0, 0x0001);                                      /* INIT */
    {
        uint32_t t0 = get_uptime_ms();
        while (!(p_rcsr(0) & 0x0100)) if (get_uptime_ms() - t0 > 500) return -1;   /* IDON */
    }
    p_wcsr(0, 0x0002);                                      /* STRT (interrupts stay off) */
    out->name = "pcnet"; out->send = p_send; out->recv = p_recv; out->link = p_link;
    return 0;
}
static int pcnet_match(uint16_t ven, uint16_t dev) { return ven == 0x1022 && (dev == 0x2000 || dev == 0x2001); }


/*
 * Intel 8255x / PRO-100 family (i82557, i82558, i82559, 82801 ICH...).
 *
 * The board is driven through the SCB window: put an address in the
 * general pointer, write a command, and the card DMAs a command block
 * (CU) or a receive frame descriptor list (RU) from memory. Two things
 * matter for real hardware:
 *
 * - PCI bus mastering must be on, or the CU reads nothing and parks;
 * - a command is only accepted once SCBCommand reads back as zero.
 *
 * Layout follows Intel's reference driver: CU/RU base loaded as 0, so
 * every link field is a plain absolute address.
 */

#define EX_CSR_STATUS   0x00    /* 16: CU state bits 7-6, RU state 5-2 */
#define EX_CSR_CMD      0x02    /* 16: command byte */
#define EX_CSR_PTR      0x04    /* 32: general pointer */
#define EX_CSR_PORT     0x08    /* 32: reset / self-test            */
#define EX_CSR_EEPROM   0x0C    /* 16: 93C46 bit-bang               */
#define EX_CSR_MDI      0x10    /* 32: MII management               */

#define EX_CU_START     0x10
#define EX_CU_RESUME    0x20
#define EX_CU_LOAD_BASE 0x60
#define EX_RU_START     0x01
#define EX_RU_RESUME    0x02
#define EX_RU_LOAD_BASE 0x06

#define EX_CB_NOP       0
#define EX_CB_IA        1
#define EX_CB_CONFIG    2
#define EX_CB_TX        4
#define EX_CB_S         0x4000   /* suspend after this block */
#define EX_CB_EL        0x8000   /* end of list              */

#define EX_RX_SLOTS     128   /* headroom: the emulated card overwrites unconsumed slots */
#define EX_RX_STRIDE    1568     /* 16-byte RFD + 1552-byte buffer */
#define EX_FRAME_MAX    1600

static uint16_t ex_io;
static int      ex_is_mmio;
static uint32_t ex_mmio;
static int      ex_phy;              /* PHY address that answered */
static uint32_t ex_st_rx, ex_st_tx, ex_st_txfail, ex_st_kick, ex_st_badst;

static uint8_t ex_cb[128]     __attribute__((aligned(16)));
static uint8_t ex_frame[EX_FRAME_MAX] __attribute__((aligned(16)));
static uint8_t ex_rx[EX_RX_SLOTS * EX_RX_STRIDE] __attribute__((aligned(16)));

static uint16_t ex_r16(uint16_t off) {
    if (ex_is_mmio) return *(volatile uint16_t*)(uintptr_t)(ex_mmio + off);
    return io_inw(ex_io + off);
}
static void ex_w16(uint16_t off, uint16_t v) {
    if (ex_is_mmio) *(volatile uint16_t*)(uintptr_t)(ex_mmio + off) = v;
    else io_outw(ex_io + off, v);
}
static uint32_t ex_r32(uint16_t off) {
    if (ex_is_mmio) return *(volatile uint32_t*)(uintptr_t)(ex_mmio + off);
    return io_inl(ex_io + off);
}
static void ex_w32(uint16_t off, uint32_t v) {
    if (ex_is_mmio) *(volatile uint32_t*)(uintptr_t)(ex_mmio + off) = v;
    else io_outl(ex_io + off, v);
}
static void ex_w8(uint16_t off, uint8_t v) {
    if (ex_is_mmio) *(volatile uint8_t*)(uintptr_t)(ex_mmio + off) = v;
    else io_outb(ex_io + off, v);
}
static uint8_t ex_r8(uint16_t off) {
    if (ex_is_mmio) return *(volatile uint8_t*)(uintptr_t)(ex_mmio + off);
    return io_inb(ex_io + off);
}
static void ex_delay(void) { io_inb(0x80); io_inb(0x80); io_inb(0x80); }

/* A command is accepted only when the card has cleared SCBCommand.
 * Skipping this wait is what makes a card look "dead": every later
 * command is written into a busy SCB and silently dropped. */
static int ex_cmd(uint8_t cmd, uint32_t ptr) {
    int i;
    for (i = 0; i < 100000; i++) {
        if ((ex_r16(EX_CSR_CMD) & 0xFF) == 0) break;
        ex_delay();
    }
    if (i >= 100000) return -1;
    if (cmd != EX_CU_RESUME && cmd != EX_RU_RESUME)
        ex_w32(EX_CSR_PTR, ptr);
    ex_w16(EX_CSR_CMD, cmd);
    return 0;
}

/* 93C46 EEPROM: word 0..2 hold the factory MAC */
#define EE_CS 0x02
#define EE_SK 0x01
#define EE_DI 0x04
#define EE_DO 0x08

static void ee_bit(int di) {
    uint8_t base = (uint8_t)(EE_CS | (di ? EE_DI : 0));
    ex_w8(EX_CSR_EEPROM, base);
    ex_delay();
    ex_w8(EX_CSR_EEPROM, (uint8_t)(base | EE_SK));
    ex_delay();
    ex_w8(EX_CSR_EEPROM, base);
    ex_delay();
}

static uint16_t ee_read(uint8_t addr) {
    uint16_t v = 0;
    int b, k;

    ex_w8(EX_CSR_EEPROM, 0x00);
    ex_delay();
    ee_bit(1);                            /* start bit  */
    ee_bit(1); ee_bit(0);                 /* opcode 10 = read */
    for (b = 5; b >= 0; b--) ee_bit((addr >> b) & 1);
    for (k = 0; k < 16; k++) {
        uint8_t base = EE_CS;
        ex_w8(EX_CSR_EEPROM, (uint8_t)(base | EE_SK));
        ex_delay();
        v = (uint16_t)((v << 1) | ((ex_r8(EX_CSR_EEPROM) & EE_DO) ? 1 : 0));
        ex_w8(EX_CSR_EEPROM, base);
        ex_delay();
    }
    ex_w8(EX_CSR_EEPROM, 0x00);
    ex_delay();
    return v;
}

/* MII (PHY) access: link status */
static uint16_t ex_mdio(uint8_t reg) {
    uint32_t v = (2u << 26) | ((uint32_t)ex_phy << 21) | ((uint32_t)reg << 16);
    int i;
    ex_w32(EX_CSR_MDI, v);
    for (i = 0; i < 1000; i++) {
        uint32_t r = ex_r32(EX_CSR_MDI);
        if (r & (1u << 28)) return (uint16_t)(r & 0xFFFF);
        ex_delay();
    }
    return 0;
}

static int ex_link(void) {
    if (ex_phy < 0) return 1;             /* no PHY: assume the cable is in */
    return (ex_mdio(1) & 0x0004) != 0;    /* BMSR: link status */
}

/* command blocks */
static void ex_cb_set(int off, uint16_t cmd, uint32_t link) {
    *(uint16_t*)(ex_cb + off + 0) = 0;
    *(uint16_t*)(ex_cb + off + 2) = cmd;
    *(uint32_t*)(ex_cb + off + 4) = link;
}

static uint32_t ex_rx_slot(int i) { return (uint32_t)(uintptr_t)(ex_rx + i * EX_RX_STRIDE); }

static void ex_arm_rx(void) {
    int i;
    for (i = 0; i < EX_RX_SLOTS; i++) {
        uint8_t* rfd = ex_rx + i * EX_RX_STRIDE;
        uint32_t next = ex_rx_slot((i + 1) % EX_RX_SLOTS);
        *(uint16_t*)(rfd + 0)  = 0;                  /* status      */
        *(uint16_t*)(rfd + 2)  = 0;                  /* command     */
        *(uint32_t*)(rfd + 4)  = next;               /* link        */
        *(uint32_t*)(rfd + 8)  = 0xFFFFFFFFu;        /* no RBD: data follows */
        *(uint16_t*)(rfd + 12) = 0;                  /* actual size */
        *(uint16_t*)(rfd + 14) = (uint16_t)(EX_RX_STRIDE - 16);
    }
}

static int ex_send(const void* frame, uint32_t len) {
    int i;
    if (len < 14 || len > EX_FRAME_MAX) return -1;
    memcpy(ex_frame, frame, len);

    /* TX CB at 48, its transmit buffer descriptor at 72 */
    ex_cb_set(48, EX_CB_TX | EX_CB_S, 0);
    *(uint32_t*)(ex_cb + 48 + 8)  = (uint32_t)(uintptr_t)(ex_cb + 72);
    *(uint16_t*)(ex_cb + 48 + 12) = 0;
    *(uint8_t*)(ex_cb + 48 + 14)  = 0;
    *(uint8_t*)(ex_cb + 48 + 15)  = 1;               /* one TBD */
    *(uint32_t*)(ex_cb + 72)      = (uint32_t)(uintptr_t)ex_frame;
    *(uint16_t*)(ex_cb + 76)      = (uint16_t)len;
    *(uint16_t*)(ex_cb + 78)      = 0x8001;           /* end of TBD list */

    if (ex_cmd(EX_CU_START, (uint32_t)(uintptr_t)(ex_cb + 48)) != 0) return -1;

    /* wait for the CU to leave the active state */
    for (i = 0; i < 100000; i++) {
        uint16_t st = ex_r16(EX_CSR_STATUS);
        if (((st >> 6) & 3) != 2) { ex_st_tx++; return 0; }   /* 2 = active */
        ex_delay();
    }
    return -1;
}

/* The RU stops itself ("no resources") once it has filled every
 * descriptor. Without this poke the card goes silent until something
 * else restarts it, which looks like a download that crawls or stalls. */
static void ex_rx_kick(int next) {
    uint16_t st = ex_r16(EX_CSR_STATUS);
    uint16_t ru = (uint16_t)((st >> 2) & 0x0F);
    if (ru != 4) {                       /* 4 = ready; 1/2 = stopped */
        ex_st_kick++;
        /* Restart at the next descriptor rather than plain resume: the
 * card also reopens its receive path, which is what releases
 * frames that piled up while it was stopped. */
        ex_cmd(EX_RU_START, ex_rx_slot(next % EX_RX_SLOTS));
    }
}

static int ex_recv(void* buf, uint32_t cap) {
    int i;
    for (i = 0; i < EX_RX_SLOTS; i++) {
        uint8_t* rfd = ex_rx + i * EX_RX_STRIDE;
        uint16_t st = *(volatile uint16_t*)(rfd + 0);
        if (!(st & 0x8000)) continue;                 /* C bit: complete */
        {
            uint32_t n = *(uint16_t*)(rfd + 12);
            if (n > EX_RX_STRIDE - 16) n = EX_RX_STRIDE - 16;
            if (n > cap) n = cap;
            memcpy(buf, rfd + 16, n);
            *(uint16_t*)(rfd + 0)  = 0;               /* re-arm */
            *(uint16_t*)(rfd + 12) = 0;
            ex_st_rx++;
            if ((st & 0x2000) == 0) ex_st_badst++;
            ex_rx_kick(i + 1);
            return (int)n;
        }
    }
    return 0;
}

static int ex_ids[] = {
    0x1029, 0x1030, 0x1031, 0x1032, 0x1033, 0x1034, 0x1035,
    0x1036, 0x1038, 0x1039, 0x103A, 0x103B, 0x103C, 0x103D,
    0x103E, 0x1050, 0x1051, 0x1059, 0x1209, 0x1229, 0x2449,
    0x2459, 0x27DC, 0
};

static int i8255x_match(uint16_t ven, uint16_t dev) {
    int k;
    if (ven != 0x8086) return 0;
    for (k = 0; ex_ids[k]; k++) if (ex_ids[k] == dev) return 1;
    return 0;
}

static int i8255x_init(uint8_t b, uint8_t d, uint8_t f, nic_t* out) {
    uint32_t bar0, bar1, cmd;
    int i;

    pci_enable_path(b);
    pci_enable(b, d, f);                     /* I/O + memory + bus master */
    cmd = pci_rd(b, d, f, 0x04);
    if (!(cmd & 0x00000004)) return -1;       /* DMA still disabled: give up */

    bar0 = pci_rd(b, d, f, 0x10);
    bar1 = pci_rd(b, d, f, 0x14);
    ex_is_mmio = 0;
    if (bar0 & 1) {
        ex_io = (uint16_t)(bar0 & ~3u);
    } else if (bar1 & 1) {
        ex_io = (uint16_t)(bar1 & ~3u);
    } else if (bar0 & ~0xFu) {
        ex_mmio = bar0 & ~0xFu;
        ex_is_mmio = 1;
    } else {
        return -1;
    }
    if (!ex_is_mmio && !ex_io) return -1;

    /* soft reset, then let the CU/RU settle */
    ex_w32(EX_CSR_PORT, 0);
    timer_delay_ms(10);
    ex_w16(EX_CSR_STATUS, 0xFFFF);           /* acknowledge anything pending */
    timer_delay_ms(5);

    /* find the PHY (usually 1) and remember it for link checks */
    ex_phy = -1;
    for (i = 1; i <= 3; i++) {
        ex_phy = i;
        if (ex_mdio(1) != 0 && ex_mdio(1) != 0xFFFF) break;
        ex_phy = -1;
    }

    /* MAC: words 0..2 of the 93C46 */
    {
        uint16_t w0 = ee_read(0), w1 = ee_read(1), w2 = ee_read(2);
        out->mac[0] = (uint8_t)(w0 & 0xFF);
        out->mac[1] = (uint8_t)(w0 >> 8);
        out->mac[2] = (uint8_t)(w1 & 0xFF);
        out->mac[3] = (uint8_t)(w1 >> 8);
        out->mac[4] = (uint8_t)(w2 & 0xFF);
        out->mac[5] = (uint8_t)(w2 >> 8);
    }
    if ((out->mac[0] | out->mac[1] | out->mac[2] | out->mac[3] |
         out->mac[4] | out->mac[5]) == 0 ||
        (out->mac[0] & out->mac[1] & out->mac[2] &
         out->mac[3] & out->mac[4] & out->mac[5]) == 0xFF) {
        /* unreadable EEPROM: fall back to a locally administered address */
        out->mac[0] = 0x52; out->mac[1] = 0x54;
        out->mac[2] = 0x00; out->mac[3] = (uint8_t)(b & 0xFF);
        out->mac[4] = (uint8_t)(d & 0xFF); out->mac[5] = (uint8_t)(f & 0xFF);
    }

    /* CU/RU base 0: every link field is then an absolute address */
    if (ex_cmd(EX_CU_LOAD_BASE, 0) != 0) return -1;
    if (ex_cmd(EX_RU_LOAD_BASE, 0) != 0) return -1;

    /* Configure: the 22 bytes Intel's own Linux driver writes for this
 * chip. The bit layout is NOT the obvious one (standard TCB, TX
 * padding, RX stripping and the retry count all live in packed
 * bitfields) - getting it wrong makes the card retransmit and the
 * link crawl, so this follows the reference driver byte for byte. */
    {
        static const uint8_t cfg[22] = {
            0x16, 0x08, 0x00, 0x00, 0x00, 0x00,  /* size, fifo, ifs  */
            0x32, 0x07, 0x01, 0x00,              /* standard TCB,   */
                                                 /* discard short,  */
                                                 /* MII mode        */
            0x2E, 0x00, 0x60, 0x00, 0xF2, 0x48,  /* preamble, IFS   */
            0x00, 0x40, 0xF2, 0x82, 0x3F, 0x05   /* pad, fc, filter */
        };
        uint8_t* c = ex_cb + 8;
        memset(ex_cb, 0, sizeof(ex_cb));
        memcpy(c, cfg, sizeof(cfg));

        ex_cb_set(0, EX_CB_CONFIG | EX_CB_S, 0);
        if (ex_cmd(EX_CU_START, (uint32_t)(uintptr_t)ex_cb) != 0) return -1;
        timer_delay_ms(2);

        /* individual address setup */
        memset(ex_cb + 32, 0, 16);
        *(uint16_t*)(ex_cb + 32 + 2) = EX_CB_IA | EX_CB_S | EX_CB_EL;
        memcpy(ex_cb + 32 + 8, out->mac, 6);
        if (ex_cmd(EX_CU_START, (uint32_t)(uintptr_t)(ex_cb + 32)) != 0) return -1;
        timer_delay_ms(2);
    }

    /* receive ring */
    ex_arm_rx();
    if (ex_cmd(EX_RU_START, ex_rx_slot(0)) != 0) return -1;

    ex_w16(EX_CSR_STATUS, 0xFFFF);           /* acknowledge, interrupts stay off */

    out->name = "i8255x";
    out->send = ex_send;
    out->recv = ex_recv;
    out->link = ex_link;
    return 0;
}

/* probe */

/* registry */
/* Every driver here keeps its state in static variables, so each one can drive ONE card;
 * a second card of the same kind is reported but left alone. */

static int e1000_match(uint16_t ven, uint16_t dev) {
    int k;
    if (ven != 0x8086) return 0;
    for (k = 0; e1000_ids[k]; k++) if (e1000_ids[k] == dev) return 1;
    return 0;
}
static int rtl8139_match(uint16_t ven, uint16_t dev) { return ven == 0x10EC && dev == 0x8139; }
static int rtl8169_match(uint16_t ven, uint16_t dev) {
    return ven == 0x10EC && (dev == 0x8169 || dev == 0x8168 || dev == 0x8167 || dev == 0x8136 || dev == 0x8161);
}
static int ar8132_match(uint16_t ven, uint16_t dev) { return ven == 0x1969 && (dev == 0x1062 || dev == 0x1063); }

typedef struct {
    const char* name;
    int (*match)(uint16_t ven, uint16_t dev);
    int (*init)(uint8_t b, uint8_t d, uint8_t f, nic_t* out);
    int used;
} nic_driver_t;

static nic_driver_t drivers[] = {
    { "e1000",      e1000_match,   e1000_init,   0 },
    { "virtio-net", virtio_match,  virtio_init,  0 },
    { "pcnet",      pcnet_match,   pcnet_init,   0 },
    { "rtl8139",    rtl8139_match, rtl8139_init, 0 },
    { "rtl8169",    rtl8169_match, rtl8169_init, 0 },
    { "ar8132",     ar8132_match,  ar8132_init,  0 },
    { "ne2000",     ne2k_match,    ne2k_init,    0 },
    { "i8255x",     i8255x_match,  i8255x_init,  0 },
};

int nic_scan_all(nic_found_t* out, int max) {
    int bus, dev, fn, n = 0, k;
    for (k = 0; k < (int)(sizeof(drivers) / sizeof(drivers[0])); k++) drivers[k].used = 0;
    for (bus = 0; bus < 256; bus++) {
        for (dev = 0; dev < 32; dev++) {
            int nfn;
            if ((pci_rd(bus, dev, 0, 0x00) & 0xFFFF) == 0xFFFF) continue;
            nfn = (pci_rd(bus, dev, 0, 0x0C) & 0x00800000) ? 8 : 1;
            for (fn = 0; fn < nfn; fn++) {
                uint32_t id = pci_rd(bus, dev, fn, 0x00), cls = pci_rd(bus, dev, fn, 0x08);
                nic_found_t* o;
                if ((id & 0xFFFF) == 0xFFFF) continue;
                if ((cls >> 24) != 0x02 || ((cls >> 16) & 0xFF) != 0x00) continue;   /* Ethernet controllers */
                if (n >= max) return n;
                o = &out[n];
                memset(o, 0, sizeof(*o));
                o->vendor = id & 0xFFFF; o->device = id >> 16;
                o->bus = bus; o->dev = dev; o->fn = fn;
                o->state = NIC_ST_NODRIVER;
                for (k = 0; k < (int)(sizeof(drivers) / sizeof(drivers[0])); k++) {
                    if (!drivers[k].match(o->vendor, o->device)) continue;
                    if (drivers[k].used) o->state = NIC_ST_SECOND;
                    else if (drivers[k].init(bus, dev, fn, &o->nic) == 0) { o->state = NIC_ST_READY; drivers[k].used = 1; }
                    else o->state = NIC_ST_FAILED;
                    o->driver = drivers[k].name;
                    break;
                }
                n++;
            }
        }
    }
    return n;
}
