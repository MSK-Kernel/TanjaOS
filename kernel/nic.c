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

/* If the BIOS left a BAR at 0, assign a safe MMIO window. */
static uint32_t pci_bar_mmio(uint8_t b, uint8_t d, uint8_t f, uint8_t off) {
    uint32_t bar = pci_rd(b, d, f, off);
    if (bar & 1) return 0;                 /* I/O BAR */
    if ((bar & ~0xFu) != 0) return bar & ~0xFu;
    pci_wr(b, d, f, off, 0xFFFFFFFFu);
    uint32_t sz = pci_rd(b, d, f, off);
    pci_wr(b, d, f, off, 0);
    if (sz == 0 || sz == 0xFFFFFFFFu) return 0;
    uint32_t size = (~(sz & ~0xFu)) + 1;
    if (size < 4096) size = 4096;
    /* 0xFE000000 is below the usual APIC/HPET area and above typical RAM. */
    static uint32_t next = 0xFE000000u;
    uint32_t addr = (next + size - 1) & ~(size - 1);
    if (addr + size > 0xFE800000u) return 0;
    pci_wr(b, d, f, off, addr);
    next = addr + size;
    return addr;
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
    pci_enable_path(b);
    pci_enable(b, d, f);
    uint32_t bar0 = pci_rd(b, d, f, 0x10);
    if (!(bar0 & 1) || !(bar0 & ~3u)) return -1;
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


/* ---------------------------------------------------- rtl8168/8169/8111 */
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
void nic_log_net(void) {
    int bus, dev, fn, any = 0;
    for (bus = 0; bus < 256; bus++) {
        for (dev = 0; dev < 32; dev++) {
            if ((pci_rd(bus, dev, 0, 0x00) & 0xFFFF) == 0xFFFF) continue;
            int nfn = (pci_rd(bus, dev, 0, 0x0C) & 0x00800000) ? 8 : 1;
            for (fn = 0; fn < nfn; fn++) {
                uint32_t id = pci_rd(bus, dev, fn, 0x00);
                uint32_t cls = pci_rd(bus, dev, fn, 0x08);
                if ((id & 0xFFFF) == 0xFFFF) continue;
                if ((cls >> 24) != 0x02) continue;
                any = 1;
                char msg[80];
                int n = 0;
                const char* pre = "PCI net ";
                while (*pre) msg[n++] = *pre++;
                /* vendor:device class */
                static const char* hexd = "0123456789abcdef";
                uint32_t v;
                msg[n++] = hexd[(id >> 12) & 0xF]; msg[n++] = hexd[(id >> 8) & 0xF];
                msg[n++] = hexd[(id >> 4) & 0xF]; msg[n++] = hexd[id & 0xF];
                msg[n++] = ':';
                v = id >> 16;
                msg[n++] = hexd[(v >> 12) & 0xF]; msg[n++] = hexd[(v >> 8) & 0xF];
                msg[n++] = hexd[(v >> 4) & 0xF]; msg[n++] = hexd[v & 0xF];
                msg[n++] = ' ';
                msg[n++] = 'c'; msg[n++] = hexd[(cls >> 20) & 0xF]; msg[n++] = hexd[(cls >> 16) & 0xF];
                msg[n] = 0;
                boot_log(msg);
            }
        }
    }
    if (!any) boot_log("PCI net: no network device on the bus");
}

/* ------------------------------------------------------- atheros AR8132 */
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
                } else if (ven == 0x1969 && (devid == 0x1062 || devid == 0x1063)) {
                    if (ar8132_init(bus, dev, fn, out) == 0) return 0;
                } else if (ven == 0x10EC) {
                    if (devid == 0x8139) {
                        if (rtl8139_init(bus, dev, fn, out) == 0) return 0;
                    } else if (devid == 0x8169 || devid == 0x8168 ||
                               devid == 0x8167 || devid == 0x8136 ||
                               devid == 0x8161) {
                        if (rtl8169_init(bus, dev, fn, out) == 0) return 0;
                    }
                }
            }
        }
    }
    return -1;
}

/* Wi-Fi cards report PCI class 0x02 (network), subclass 0x80 (other). */
int nic_wifi_scan(wifi_hw_t* out, int max) {
    int bus, dev, fn, n = 0;
    for (bus = 0; bus < 256; bus++) {
        for (dev = 0; dev < 32; dev++) {
            uint32_t id0 = pci_rd(bus, dev, 0, 0x00);
            if ((id0 & 0xFFFF) == 0xFFFF) continue;
            int nfn = (pci_rd(bus, dev, 0, 0x0C) & 0x00800000) ? 8 : 1;
            for (fn = 0; fn < nfn; fn++) {
                uint32_t id = pci_rd(bus, dev, fn, 0x00);
                uint32_t cls = pci_rd(bus, dev, fn, 0x08);
                if ((id & 0xFFFF) == 0xFFFF) continue;
                if ((cls >> 24) == 0x02 && ((cls >> 16) & 0xFF) == 0x80 && n < max) {
                    out[n].vendor = id & 0xFFFF; out[n].device = id >> 16;
                    out[n].bus = bus; out[n].dev = dev; out[n].fn = fn;
                    n++;
                }
            }
        }
    }
    return n;
}
