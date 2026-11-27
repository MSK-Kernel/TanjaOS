/* NVMe, read-only: the first namespace of the first controller.
 *
 * Polled (no interrupts): one admin queue pair and one I/O queue pair, 4 KiB
 * pages, data moved through a page-aligned bounce buffer so that PRP lists
 * stay trivial. Namespaces with 512-byte or 4096-byte logical blocks work;
 * the device is presented to the rest of the kernel as 512-byte sectors.
 * The BAR must lie below 4 GiB (this is a 32-bit kernel without PAE).
 */
#include <stdint.h>
#include <stddef.h>
#include "../include/nvme.h"

extern uint32_t get_uptime_ms(void);

static inline void outl(uint16_t port, uint32_t val) {
    asm volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    asm volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
extern void print(const char* s);

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

static uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    outl(PCI_CONFIG_ADDRESS, 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
                             ((uint32_t)func << 8) | (off & 0xFC));
    return inl(PCI_CONFIG_DATA);
}
static void pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint32_t v) {
    outl(PCI_CONFIG_ADDRESS, 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
                             ((uint32_t)func << 8) | (off & 0xFC));
    outl(PCI_CONFIG_DATA, v);
}

/* controller registers */
#define REG_CAP   0x00
#define REG_CC    0x14
#define REG_CSTS  0x1C
#define REG_AQA   0x24
#define REG_ASQ   0x28
#define REG_ACQ   0x30

#define QD 16                           /* entries per queue (plenty for polled single commands) */
#define XFER_PAGES 8                    /* bounce buffer: 32 KiB */

struct sqe { uint32_t d[16]; };
struct cqe { uint32_t d[4]; };

static volatile uint8_t* regs;
static uint32_t dstrd;                  /* doorbell stride shift */
static struct sqe asq[QD] __attribute__((aligned(4096)));
static struct cqe acq[QD] __attribute__((aligned(4096)));
static struct sqe isq[QD] __attribute__((aligned(4096)));
static struct cqe icq[QD] __attribute__((aligned(4096)));
static uint8_t xfer[XFER_PAGES * 4096] __attribute__((aligned(4096)));
static uint64_t prp_list[512] __attribute__((aligned(4096)));
static uint8_t ident[4096] __attribute__((aligned(4096)));

static uint32_t a_tail, a_head, a_phase = 1;
static uint32_t i_tail, i_head, i_phase = 1;
static uint32_t cid_next = 1;

static int ready = 0;
static uint64_t ns_blocks;              /* in logical blocks */
static uint32_t lb_shift = 9;           /* log2 of the logical block size */
static char model[48];

static uint32_t rd32m(uint32_t off) { return *(volatile uint32_t*)(regs + off); }
static void wr32m(uint32_t off, uint32_t v) { *(volatile uint32_t*)(regs + off) = v; }
static void wr64m(uint32_t off, uint64_t v) { wr32m(off, (uint32_t)v); wr32m(off + 4, (uint32_t)(v >> 32)); }

static void ring(uint32_t qid, int is_cq, uint32_t value) {
    wr32m(0x1000 + (2 * qid + (is_cq ? 1 : 0)) * (4u << dstrd), value);
}

/* submit one command on a queue and poll for its completion; 0 = success */
static int submit(int admin, struct sqe* sq, struct cqe* cq, uint32_t* tail, uint32_t* head,
                  uint32_t* phase, const struct sqe* cmd, uint32_t timeout_ms)
{
    uint32_t qid = admin ? 0 : 1, start, i;
    struct sqe* slot = &sq[*tail];
    uint32_t cid = cid_next++ & 0xFFFF;
    for (i = 0; i < 16; i++) slot->d[i] = cmd->d[i];
    slot->d[0] = (cmd->d[0] & 0xFFFF) | (cid << 16);
    *tail = (*tail + 1) % QD;
    ring(qid, 0, *tail);

    start = get_uptime_ms();
    for (;;) {
        volatile struct cqe* e = &cq[*head];
        uint32_t d3 = e->d[3];
        if (((d3 >> 16) & 1u) == *phase) {
            uint32_t status = (d3 >> 17) & 0x7FFF;
            *head = (*head + 1) % QD;
            if (*head == 0) *phase ^= 1u;
            ring(qid, 1, *head);
            return status == 0 ? 0 : -1;
        }
        if (get_uptime_ms() - start > timeout_ms) return -1;
    }
}

static int admin(const struct sqe* c) { return submit(1, asq, acq, &a_tail, &a_head, &a_phase, c, 3000); }

static void zero(void* p, uint32_t n) { uint8_t* b = (uint8_t*)p; while (n--) *b++ = 0; }

/* -1 = none; otherwise fills bus/dev/func and the BAR */
static int find_nvme(uint8_t* bus, uint8_t* dev, uint8_t* func, uint32_t* bar_lo, uint32_t* bar_hi)
{
    uint32_t b, d, f;
    for (b = 0; b < 256; b++)
        for (d = 0; d < 32; d++) {
            uint32_t id0 = pci_read32((uint8_t)b, (uint8_t)d, 0, 0);
            uint32_t maxf;
            if ((id0 & 0xFFFF) == 0xFFFF) continue;
            maxf = (pci_read32((uint8_t)b, (uint8_t)d, 0, 0x0C) >> 16) & 0x80 ? 8 : 1;
            for (f = 0; f < maxf; f++) {
                uint32_t cls;
                if ((pci_read32((uint8_t)b, (uint8_t)d, (uint8_t)f, 0) & 0xFFFF) == 0xFFFF) continue;
                cls = pci_read32((uint8_t)b, (uint8_t)d, (uint8_t)f, 0x08);
                if ((cls >> 8) == 0x010802u) {                    /* mass storage / NVM / NVMe */
                    *bus = (uint8_t)b; *dev = (uint8_t)d; *func = (uint8_t)f;
                    *bar_lo = pci_read32((uint8_t)b, (uint8_t)d, (uint8_t)f, 0x10);
                    *bar_hi = ((*bar_lo & 6u) == 4u) ? pci_read32((uint8_t)b, (uint8_t)d, (uint8_t)f, 0x14) : 0;
                    return 0;
                }
            }
        }
    return -1;
}

int nvme_init(void)
{
    uint8_t bus, dev, func;
    uint32_t bar_lo, bar_hi, cmd, i, timeout, cc;
    uint64_t cap;
    struct sqe c;

    if (ready) return 0;
    if (find_nvme(&bus, &dev, &func, &bar_lo, &bar_hi) != 0) return -1;
    if (bar_hi != 0) { print("nvme: controller is mapped above 4 GiB, which this kernel cannot reach\n"); return -1; }
    regs = (volatile uint8_t*)(uintptr_t)(bar_lo & 0xFFFFFFF0u);
    if (!regs) return -1;

    cmd = pci_read32(bus, dev, func, 0x04);                       /* memory space + bus master */
    pci_write32(bus, dev, func, 0x04, cmd | 0x06u);

    cap = (uint64_t)rd32m(REG_CAP) | ((uint64_t)rd32m(REG_CAP + 4) << 32);
    dstrd = (uint32_t)((cap >> 32) & 0xF);
    timeout = (uint32_t)(((cap >> 24) & 0xFF) * 500u);
    if (timeout < 1000) timeout = 1000;
    if (((cap >> 48) & 0xF) > 0) return -1;                        /* needs pages bigger than 4 KiB */

    /* reset, then bring it up with our admin queues */
    wr32m(REG_CC, rd32m(REG_CC) & ~1u);
    { uint32_t t = get_uptime_ms(); while ((rd32m(REG_CSTS) & 1u) && get_uptime_ms() - t < timeout) { } }
    if (rd32m(REG_CSTS) & 1u) return -1;

    zero(asq, sizeof asq); zero(acq, sizeof acq); zero(isq, sizeof isq); zero(icq, sizeof icq);
    a_tail = a_head = i_tail = i_head = 0; a_phase = i_phase = 1;
    wr32m(REG_AQA, ((QD - 1) << 16) | (QD - 1));
    wr64m(REG_ASQ, (uint64_t)(uintptr_t)asq);
    wr64m(REG_ACQ, (uint64_t)(uintptr_t)acq);
    cc = 1u | (6u << 16) | (4u << 20);                             /* EN, 64-byte SQE, 16-byte CQE, 4 KiB pages */
    wr32m(REG_CC, cc);
    { uint32_t t = get_uptime_ms(); while (!(rd32m(REG_CSTS) & 1u) && get_uptime_ms() - t < timeout) { } }
    if (!(rd32m(REG_CSTS) & 1u)) return -1;

    /* Identify controller: the model string */
    zero(&c, sizeof c);
    c.d[0] = 0x06; c.d[6] = (uint32_t)(uintptr_t)ident; c.d[10] = 1;      /* CNS 1 */
    if (admin(&c) != 0) return -1;
    for (i = 0; i < 40; i++) model[i] = (char)ident[24 + i];
    model[40] = 0;
    for (i = 40; i > 0 && (model[i - 1] == ' ' || model[i - 1] == 0); i--) model[i - 1] = 0;

    /* Identify namespace 1: size and block size */
    zero(&c, sizeof c);
    c.d[0] = 0x06; c.d[1] = 1; c.d[6] = (uint32_t)(uintptr_t)ident; c.d[10] = 0;   /* CNS 0, NSID 1 */
    if (admin(&c) != 0) return -1;
    ns_blocks = (uint64_t)(ident[0] | (ident[1] << 8) | (ident[2] << 16) | ((uint32_t)ident[3] << 24)) |
                ((uint64_t)(ident[4] | (ident[5] << 8) | (ident[6] << 16) | ((uint32_t)ident[7] << 24)) << 32);
    {
        uint32_t fmt = ident[26] & 0x0F;
        lb_shift = ident[128 + fmt * 4 + 2];                       /* LBADS */
    }
    if (ns_blocks == 0 || (lb_shift != 9 && lb_shift != 12)) return -1;

    /* I/O queue pair 1 */
    zero(&c, sizeof c);
    c.d[0] = 0x05; c.d[6] = (uint32_t)(uintptr_t)icq;
    c.d[10] = ((QD - 1) << 16) | 1u; c.d[11] = 1u;                 /* create CQ 1, physically contiguous */
    if (admin(&c) != 0) return -1;
    zero(&c, sizeof c);
    c.d[0] = 0x01; c.d[6] = (uint32_t)(uintptr_t)isq;
    c.d[10] = ((QD - 1) << 16) | 1u; c.d[11] = (1u << 16) | 1u;   /* create SQ 1 -> CQ 1 */
    if (admin(&c) != 0) return -1;

    ready = 1;
    return 0;
}

const char* nvme_model(void) { return model; }
uint64_t nvme_sectors(void) { return ready ? (ns_blocks << (lb_shift - 9)) : 0; }

/* read whole logical blocks into the bounce buffer: nblk <= XFER_PAGES*4096 / block size */
static int read_blocks(uint64_t slba, uint32_t nblk)
{
    struct sqe c;
    uint32_t bytes = nblk << lb_shift, pages = (bytes + 4095) / 4096, k;
    zero(&c, sizeof c);
    c.d[0] = 0x02;                                                 /* READ */
    c.d[1] = 1;                                                    /* NSID */
    c.d[6] = (uint32_t)(uintptr_t)xfer;                            /* PRP1 */
    if (pages == 2) {
        c.d[8] = (uint32_t)(uintptr_t)(xfer + 4096);               /* PRP2 = second page */
    } else if (pages > 2) {
        for (k = 1; k < pages; k++) prp_list[k - 1] = (uint64_t)(uintptr_t)(xfer + k * 4096);
        c.d[8] = (uint32_t)(uintptr_t)prp_list;                    /* PRP2 = list */
    }
    c.d[10] = (uint32_t)slba; c.d[11] = (uint32_t)(slba >> 32);
    c.d[12] = nblk - 1;
    return submit(0, isq, icq, &i_tail, &i_head, &i_phase, &c, 10000);
}

int nvme_read(uint64_t lba, uint32_t count, void* buf)
{
    uint8_t* out = (uint8_t*)buf;
    uint32_t bs = 1u << lb_shift, per_chunk = (XFER_PAGES * 4096) >> lb_shift;
    if (!ready || !buf || !count) return -1;
    if (lba + count > nvme_sectors()) return -1;
    while (count) {
        uint64_t byte_off = lba * 512u;
        uint64_t first = byte_off >> lb_shift;
        uint32_t head = (uint32_t)(byte_off & (bs - 1));
        uint32_t want = count * 512u, take, nblk, i;
        nblk = (head + want + bs - 1) >> lb_shift;
        if (nblk > per_chunk) nblk = per_chunk;
        take = nblk * bs - head;
        if (take > want) take = want;
        if (read_blocks(first, nblk) != 0 && read_blocks(first, nblk) != 0) return -1;
        for (i = 0; i < take; i++) out[i] = xfer[head + i];
        out += take; lba += take / 512; count -= take / 512;
    }
    return 0;
}
