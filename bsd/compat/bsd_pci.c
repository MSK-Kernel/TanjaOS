/* bsd_pci.c - PCI, bus_space and bus_dma for the OpenBSD Wi-Fi drivers on TanjaOS.
 *
 * TanjaOS runs in ring 0 with no paging, so a buffer's address is its DMA address and a
 * BAR's physical address is directly usable. Interrupts are not delivered: handlers
 * registered here are polled from bsd_poll(). DMA memory comes from a static pool.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/errno.h>
#include <machine/bus.h>
#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>

extern int printf(const char* fmt, ...);

static inline void io_outl(unsigned short p, unsigned int v) { __asm volatile("outl %0,%1" : : "a"(v), "Nd"(p)); }
static inline unsigned int io_inl(unsigned short p) { unsigned int v; __asm volatile("inl %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline void io_outb(unsigned short p, unsigned char v) { __asm volatile("outb %0,%1" : : "a"(v), "Nd"(p)); }
static inline void io_outw(unsigned short p, unsigned short v) { __asm volatile("outw %0,%1" : : "a"(v), "Nd"(p)); }
static inline unsigned char io_inb(unsigned short p) { unsigned char v; __asm volatile("inb %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline unsigned short io_inw(unsigned short p) { unsigned short v; __asm volatile("inw %1,%0" : "=a"(v) : "Nd"(p)); return v; }

/* ------------------------------------------------------ PCI config */

pcireg_t pci_conf_read(pci_chipset_tag_t pc, pcitag_t tag, int reg) {
    io_outl(0xCF8, tag.mode1 | (reg & 0xFC));
    return io_inl(0xCFC);
}
void pci_conf_write(pci_chipset_tag_t pc, pcitag_t tag, int reg, pcireg_t val) {
    io_outl(0xCF8, tag.mode1 | (reg & 0xFC));
    io_outl(0xCFC, val);
}

int pci_get_capability(pci_chipset_tag_t pc, pcitag_t tag, int capid, int *offset, pcireg_t *value) {
    pcireg_t r;
    unsigned int ofs;
    int guard = 48;
    if (!(pci_conf_read(pc, tag, PCI_COMMAND_STATUS_REG) & PCI_STATUS_CAPLIST_SUPPORT)) return 0;
    ofs = PCI_CAPLIST_PTR(pci_conf_read(pc, tag, PCI_CAPLISTPTR_REG));
    while (ofs != 0 && guard--) {
        r = pci_conf_read(pc, tag, ofs);
        if (PCI_CAPLIST_CAP(r) == capid) {
            if (offset) *offset = ofs;
            if (value) *value = r;
            return 1;
        }
        ofs = PCI_CAPLIST_NEXT(r);
    }
    return 0;
}

int pci_matchbyid(struct pci_attach_args *pa, const struct pci_matchid *ids, int nent) {
    int i;
    for (i = 0; i < nent; i++)
        if (PCI_VENDOR(pa->pa_id) == ids[i].pm_vid && PCI_PRODUCT(pa->pa_id) == ids[i].pm_pid) return 1;
    return 0;
}

pcireg_t pci_mapreg_type(pci_chipset_tag_t pc, pcitag_t tag, int reg) {
    return PCI_MAPREG_TYPE(pci_conf_read(pc, tag, reg));
}

/* works out the size of a BAR and returns its (identity-mapped) base */
int pci_mapreg_map(struct pci_attach_args *pa, int reg, pcireg_t type, int flags,
    bus_space_tag_t *tagp, bus_space_handle_t *handlep, bus_addr_t *basep, bus_size_t *sizep, bus_size_t maxsize) {
    pcireg_t orig = pci_conf_read(pa->pa_pc, pa->pa_tag, reg), mask, size;
    pcireg_t cmd;
    unsigned long base;
    int is_io = PCI_MAPREG_TYPE(orig) == PCI_MAPREG_TYPE_IO;

    if (is_io) {
        pci_conf_write(pa->pa_pc, pa->pa_tag, reg, 0xFFFFFFFF);
        mask = pci_conf_read(pa->pa_pc, pa->pa_tag, reg);
        pci_conf_write(pa->pa_pc, pa->pa_tag, reg, orig);
        size = ~(mask & PCI_MAPREG_IO_ADDR_MASK) + 1;
        base = orig & PCI_MAPREG_IO_ADDR_MASK;
    } else {
        if (PCI_MAPREG_MEM_TYPE(orig) == PCI_MAPREG_MEM_TYPE_64BIT &&
            pci_conf_read(pa->pa_pc, pa->pa_tag, reg + 4) != 0) {
            printf("bsd: BAR above 4 GB, not usable\n");
            return 1;
        }
        pci_conf_write(pa->pa_pc, pa->pa_tag, reg, 0xFFFFFFFF);
        mask = pci_conf_read(pa->pa_pc, pa->pa_tag, reg);
        pci_conf_write(pa->pa_pc, pa->pa_tag, reg, orig);
        size = ~(mask & PCI_MAPREG_MEM_ADDR_MASK) + 1;
        base = orig & PCI_MAPREG_MEM_ADDR_MASK;
    }
    if (base == 0 || size == 0) return 1;
    cmd = pci_conf_read(pa->pa_pc, pa->pa_tag, PCI_COMMAND_STATUS_REG);
    cmd |= is_io ? PCI_COMMAND_IO_ENABLE : PCI_COMMAND_MEM_ENABLE;
    cmd |= PCI_COMMAND_MASTER_ENABLE;
    pci_conf_write(pa->pa_pc, pa->pa_tag, PCI_COMMAND_STATUS_REG, cmd);
    if (tagp) *tagp = is_io ? I386_BUS_SPACE_IO : I386_BUS_SPACE_MEM;
    if (handlep) *handlep = base;
    if (basep) *basep = base;
    if (sizep) *sizep = size;
    return 0;
}

/* ------------------------------------------------------- interrupts */

struct irq_h { int (*fn)(void *); void *arg; };
#define MAX_IRQH 4
static struct irq_h irqh[MAX_IRQH];

int pci_intr_map(struct pci_attach_args *pa, pci_intr_handle_t *ih) { memset(ih, 0, sizeof(*ih)); return 0; }
int pci_intr_map_msi(struct pci_attach_args *pa, pci_intr_handle_t *ih) { return 1; }   /* MSI unsupported: use legacy */
const char *pci_intr_string(pci_chipset_tag_t pc, pci_intr_handle_t ih) { return "polled"; }

void *pci_intr_establish(pci_chipset_tag_t pc, pci_intr_handle_t ih, int level, int (*func)(void *), void *arg, const char *name) {
    int i;
    for (i = 0; i < MAX_IRQH; i++)
        if (!irqh[i].fn) { irqh[i].fn = func; irqh[i].arg = arg; return &irqh[i]; }
    return NULL;
}

void bsd_run_irq_handlers(void) {
    int i;
    for (i = 0; i < MAX_IRQH; i++) if (irqh[i].fn) irqh[i].fn(irqh[i].arg);
}

/* ------------------------------------------------------- bus_space */

#define MEM8(h, o)  (*(volatile u_int8_t  *)((h) + (o)))
#define MEM16(h, o) (*(volatile u_int16_t *)((h) + (o)))
#define MEM32(h, o) (*(volatile u_int32_t *)((h) + (o)))

static u_int8_t  m_r1(bus_space_handle_t h, bus_size_t o) { return MEM8(h, o); }
static u_int16_t m_r2(bus_space_handle_t h, bus_size_t o) { return MEM16(h, o); }
static u_int32_t m_r4(bus_space_handle_t h, bus_size_t o) { return MEM32(h, o); }
static void m_w1(bus_space_handle_t h, bus_size_t o, u_int8_t v)  { MEM8(h, o) = v; }
static void m_w2(bus_space_handle_t h, bus_size_t o, u_int16_t v) { MEM16(h, o) = v; }
static void m_w4(bus_space_handle_t h, bus_size_t o, u_int32_t v) { MEM32(h, o) = v; }
static void m_rr4(bus_space_handle_t h, bus_size_t o, u_int32_t *a, bus_size_t c) { while (c--) { *a++ = MEM32(h, o); o += 4; } }
static void m_wr4(bus_space_handle_t h, bus_size_t o, const u_int32_t *a, bus_size_t c) { while (c--) { MEM32(h, o) = *a++; o += 4; } }
static void m_rr1(bus_space_handle_t h, bus_size_t o, u_int8_t *a, bus_size_t c) { while (c--) *a++ = MEM8(h, o++); }
static void m_wr1(bus_space_handle_t h, bus_size_t o, const u_int8_t *a, bus_size_t c) { while (c--) MEM8(h, o++) = *a++; }
static void m_rm4(bus_space_handle_t h, bus_size_t o, u_int32_t *a, bus_size_t c) { while (c--) *a++ = MEM32(h, o); }
static void m_wm4(bus_space_handle_t h, bus_size_t o, const u_int32_t *a, bus_size_t c) { while (c--) MEM32(h, o) = *a++; }
static void *m_vaddr(bus_space_handle_t h) { return (void *)h; }

static u_int8_t  i_r1(bus_space_handle_t h, bus_size_t o) { return io_inb(h + o); }
static u_int16_t i_r2(bus_space_handle_t h, bus_size_t o) { return io_inw(h + o); }
static u_int32_t i_r4(bus_space_handle_t h, bus_size_t o) { return io_inl(h + o); }
static void i_w1(bus_space_handle_t h, bus_size_t o, u_int8_t v)  { io_outb(h + o, v); }
static void i_w2(bus_space_handle_t h, bus_size_t o, u_int16_t v) { io_outw(h + o, v); }
static void i_w4(bus_space_handle_t h, bus_size_t o, u_int32_t v) { io_outl(h + o, v); }

const struct i386_bus_space_ops i386_bus_space_mem_ops = {
    .read_1 = m_r1, .read_2 = m_r2, .read_4 = m_r4,
    .write_1 = m_w1, .write_2 = m_w2, .write_4 = m_w4,
    .read_region_1 = m_rr1, .read_region_4 = m_rr4,
    .write_region_1 = m_wr1, .write_region_4 = m_wr4,
    .read_multi_4 = m_rm4, .write_multi_4 = m_wm4,
    .vaddr = m_vaddr,
};
const struct i386_bus_space_ops i386_bus_space_io_ops = {
    .read_1 = i_r1, .read_2 = i_r2, .read_4 = i_r4,
    .write_1 = i_w1, .write_2 = i_w2, .write_4 = i_w4,
};

/* --------------------------------------------------------- bus_dma */

#define DMA_POOL (4u << 20)
static unsigned char dma_pool[DMA_POOL] __attribute__((aligned(4096)));
static unsigned int dma_top;

static int d_map_create(bus_dma_tag_t t, bus_size_t size, int nseg, bus_size_t maxseg, bus_size_t boundary, int flags, bus_dmamap_t *mapp) {
    bus_dmamap_t m = malloc(sizeof(*m) + sizeof(bus_dma_segment_t) * (nseg > 0 ? nseg : 1), M_DEVBUF, M_NOWAIT | M_ZERO);
    if (!m) return ENOMEM;
    m->_dm_size = size; m->_dm_segcnt = nseg; m->_dm_maxsegsz = maxseg; m->_dm_boundary = boundary; m->_dm_flags = flags;
    *mapp = m;
    return 0;
}
static void d_map_destroy(bus_dma_tag_t t, bus_dmamap_t m) { free(m, M_DEVBUF, 0); }

static int d_map_load(bus_dma_tag_t t, bus_dmamap_t m, void *buf, bus_size_t len, struct proc *p, int flags) {
    if (m->_dm_segcnt < 1) return EFBIG;
    m->dm_segs[0].ds_addr = (bus_addr_t)buf;
    m->dm_segs[0].ds_len = len;
    m->dm_nsegs = 1;
    m->dm_mapsize = len;
    return 0;
}

static int d_map_load_mbuf(bus_dma_tag_t t, bus_dmamap_t m, struct mbuf *chain, int flags) {
    int n = 0;
    struct mbuf *mb;
    m->dm_nsegs = 0; m->dm_mapsize = 0;
    for (mb = chain; mb; mb = mb->m_next) {
        if (mb->m_len == 0) continue;
        if (n >= m->_dm_segcnt) return EFBIG;
        m->dm_segs[n].ds_addr = (bus_addr_t)mb->m_data;
        m->dm_segs[n].ds_len = mb->m_len;
        m->dm_mapsize += mb->m_len;
        n++;
    }
    m->dm_nsegs = n;
    return 0;
}

static void d_map_unload(bus_dma_tag_t t, bus_dmamap_t m) { m->dm_nsegs = 0; m->dm_mapsize = 0; }
static void d_map_sync(bus_dma_tag_t t, bus_dmamap_t m, bus_addr_t off, bus_size_t len, int ops) { __asm volatile("mfence" ::: "memory"); }

static int d_mem_alloc(bus_dma_tag_t t, bus_size_t size, bus_size_t align, bus_size_t boundary, bus_dma_segment_t *segs, int nsegs, int *rsegs, int flags) {
    unsigned long a;
    if (align < 16) align = 16;
    a = ((unsigned long)dma_pool + dma_top + align - 1) & ~(unsigned long)(align - 1);
    if (boundary && (a / boundary) != ((a + size - 1) / boundary)) a = (a + boundary - 1) & ~(unsigned long)(boundary - 1);
    if (a + size > (unsigned long)dma_pool + DMA_POOL) { printf("bsd: DMA pool exhausted\n"); return ENOMEM; }
    dma_top = (unsigned int)(a + size - (unsigned long)dma_pool);
    memset((void *)a, 0, size);
    segs[0].ds_addr = a; segs[0].ds_len = size;
    *rsegs = 1;
    return 0;
}
static void d_mem_free(bus_dma_tag_t t, bus_dma_segment_t *segs, int nsegs) { }
static int d_mem_map(bus_dma_tag_t t, bus_dma_segment_t *segs, int nsegs, size_t size, caddr_t *kvap, int flags) { *kvap = (caddr_t)segs[0].ds_addr; return 0; }
static void d_mem_unmap(bus_dma_tag_t t, caddr_t kva, size_t size) { }

struct bus_dma_tag bsd_dma_tag = {
    ._cookie = 0,
    ._dmamap_create = d_map_create, ._dmamap_destroy = d_map_destroy,
    ._dmamap_load = d_map_load, ._dmamap_load_mbuf = d_map_load_mbuf,
    ._dmamap_unload = d_map_unload, ._dmamap_sync = d_map_sync,
    ._dmamem_alloc = d_mem_alloc, ._dmamem_free = d_mem_free,
    ._dmamem_map = d_mem_map, ._dmamem_unmap = d_mem_unmap,
};

/* ------------------------------------------------------------ selftest */

#define CHK(c, msg) do { if (c) pass++; else { fail++; printf("  FAIL: %s\n", msg); } } while (0)
static int st_irq_cb(void *a) { (*(int *)a)++; return 1; }
int bsd_pci_selftest(void) {
    int pass = 0, fail = 0, rsegs, i;
    bus_dma_segment_t seg;
    caddr_t kva;
    bus_dmamap_t map;
    static volatile u_int32_t regs[8];
    bus_space_tag_t t = I386_BUS_SPACE_MEM;
    bus_space_handle_t h = (bus_space_handle_t)regs;
    struct mbuf *m, *m2;
    pcitag_t tag;

    CHK(bus_dmamem_alloc(&bsd_dma_tag, 8192, 4096, 0, &seg, 1, &rsegs, BUS_DMA_NOWAIT | BUS_DMA_ZERO) == 0 && rsegs == 1 && (seg.ds_addr & 4095) == 0, "dmamem_alloc is 4K aligned");
    CHK(bus_dmamem_map(&bsd_dma_tag, &seg, 1, 8192, &kva, BUS_DMA_NOWAIT) == 0 && (bus_addr_t)kva == seg.ds_addr && kva[0] == 0 && kva[8191] == 0, "dmamem_map is zeroed and identity mapped");
    CHK(bus_dmamap_create(&bsd_dma_tag, 8192, 1, 8192, 0, BUS_DMA_NOWAIT, &map) == 0, "dmamap_create");
    CHK(bus_dmamap_load(&bsd_dma_tag, map, kva, 8192, NULL, BUS_DMA_NOWAIT) == 0 && map->dm_nsegs == 1 && map->dm_segs[0].ds_addr == seg.ds_addr, "dmamap_load");
    bus_dmamap_sync(&bsd_dma_tag, map, 0, 8192, BUS_DMASYNC_PREWRITE);
    bus_dmamap_unload(&bsd_dma_tag, map);
    bus_dmamap_destroy(&bsd_dma_tag, map);

    m = m_gethdr(M_DONTWAIT, MT_DATA); m->m_len = 10; m->m_pkthdr.len = 20;
    m2 = m_get(M_DONTWAIT, MT_DATA); m2->m_len = 10; m->m_next = m2;
    bus_dmamap_create(&bsd_dma_tag, 2048, 4, 2048, 0, BUS_DMA_NOWAIT, &map);
    CHK(bus_dmamap_load_mbuf(&bsd_dma_tag, map, m, BUS_DMA_NOWAIT) == 0 && map->dm_nsegs == 2 && map->dm_mapsize == 20, "dmamap_load_mbuf: one segment per mbuf");
    bus_dmamap_destroy(&bsd_dma_tag, map);
    bus_dmamap_create(&bsd_dma_tag, 2048, 1, 2048, 0, BUS_DMA_NOWAIT, &map);
    CHK(bus_dmamap_load_mbuf(&bsd_dma_tag, map, m, BUS_DMA_NOWAIT) == EFBIG, "dmamap_load_mbuf: too many segments -> EFBIG");
    bus_dmamap_destroy(&bsd_dma_tag, map);
    m_freem(m);

    for (i = 0; i < 8; i++) regs[i] = 0;
    bus_space_write_4(t, h, 4, 0xDEADBEEF);
    CHK(regs[1] == 0xDEADBEEF && bus_space_read_4(t, h, 4) == 0xDEADBEEF, "bus_space 32-bit read/write");
    bus_space_write_1(t, h, 1, 0x5A);
    CHK(bus_space_read_1(t, h, 1) == 0x5A && bus_space_read_2(t, h, 0) == 0x5A00, "bus_space 8/16-bit access");

    tag.mode1 = 0x80000000u;                         /* bus 0, device 0, function 0 */
    CHK((pci_conf_read(NULL, tag, 0) & 0xFFFF) != 0xFFFF, "pci_conf_read finds the host bridge");
    {
        struct pci_attach_args pa;
        static const struct pci_matchid ids[] = { { 0x8086, 0x2526 }, { 0x8086, 0x9df0 } };
        memset(&pa, 0, sizeof(pa));
        pa.pa_id = (0x2526u << 16) | 0x8086;
        CHK(pci_matchbyid(&pa, ids, 2) == 1, "pci_matchbyid matches 8086:2526");
        pa.pa_id = (0x1234u << 16) | 0x8086;
        CHK(pci_matchbyid(&pa, ids, 2) == 0, "pci_matchbyid rejects other ids");
    }
    {
        static int fired;
        extern void bsd_poll(void);
        void *ih = pci_intr_establish(NULL, (pci_intr_handle_t){ 0 }, IPL_NET, st_irq_cb, &fired, "t");
        bsd_poll();
        CHK(ih != NULL && fired >= 1, "registered interrupt handler is polled");
    }

    printf("bsd selftest (pci/dma): %d passed, %d failed\n", pass, fail);
    return fail;
}
