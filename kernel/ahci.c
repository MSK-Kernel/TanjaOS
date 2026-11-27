#include <stdint.h>
#include "../include/ahci.h"

extern uint32_t get_uptime_ms(void);

static inline void outl(uint16_t port, uint32_t val) {
    asm volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    asm volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// PCI config space access (legacy I/O-port mechanism, universally
// supported since well before AHCI existed)

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

static uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    uint32_t address = 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11)
                      | ((uint32_t)func << 8) | (offset & 0xFC);
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

// AHCI register layout (AHCI 1.3.1 spec)

typedef struct {
    volatile uint32_t cap;
    volatile uint32_t ghc;
    volatile uint32_t is;
    volatile uint32_t pi;
    volatile uint32_t vs;
    volatile uint32_t ccc_ctl;
    volatile uint32_t ccc_pts;
    volatile uint32_t em_loc;
    volatile uint32_t em_ctl;
    volatile uint32_t cap2;
    volatile uint32_t bohc;
} __attribute__((packed)) hba_ghc_t;

typedef struct {
    volatile uint32_t clb;
    volatile uint32_t clbu;
    volatile uint32_t fb;
    volatile uint32_t fbu;
    volatile uint32_t is;
    volatile uint32_t ie;
    volatile uint32_t cmd;
    volatile uint32_t reserved0;
    volatile uint32_t tfd;
    volatile uint32_t sig;
    volatile uint32_t ssts;
    volatile uint32_t sctl;
    volatile uint32_t serr;
    volatile uint32_t sact;
    volatile uint32_t ci;
    volatile uint32_t sntf;
    volatile uint32_t fbs;
} __attribute__((packed)) hba_port_t;

#define GHC_AE        (1u << 31)
#define PORT_CMD_ST   (1u << 0)
#define PORT_CMD_FRE  (1u << 4)
#define PORT_CMD_FR   (1u << 14)
#define PORT_CMD_CR   (1u << 15)
#define TFD_BSY       (1u << 7)
#define TFD_DRQ       (1u << 3)
#define TFD_ERR       (1u << 0)

typedef struct {
    volatile uint32_t dw0; // bits0-4 CFL, bit6 W, bits16-31 PRDTL
    volatile uint32_t prdbc;
    volatile uint32_t ctba;
    volatile uint32_t ctbau;
    volatile uint32_t reserved[4];
} __attribute__((packed)) hba_cmd_header_t;

typedef struct {
    volatile uint32_t dba;
    volatile uint32_t dbau;
    volatile uint32_t reserved;
    volatile uint32_t dbc_ic; // bits0-21 byte count-1, bit31 IOC
} __attribute__((packed)) hba_prdt_entry_t;

#define AHCI_PRDT_ENTRIES 1
typedef struct {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t reserved[48];
    hba_prdt_entry_t prdt[AHCI_PRDT_ENTRIES];
} __attribute__((packed)) hba_cmd_table_t;

// Statically allocated, correctly-aligned per-port structures. Only
// one port is ever active (the first present plain-SATA drive found),
// so one set is enough - no dynamic allocation needed.
static hba_cmd_header_t cmd_list[32] __attribute__((aligned(1024)));
static uint8_t fis_recv[256] __attribute__((aligned(256)));
static hba_cmd_table_t cmd_table __attribute__((aligned(128)));
static uint16_t identify_buf[256] __attribute__((aligned(2)));

static hba_ghc_t* ghc = 0;
static hba_port_t* port = 0;
static int ahci_available = 0;
static uint64_t drive_sectors = 0;

// PCI enumeration

static int find_ahci_pci_device(uint32_t* out_abar) {
    uint32_t bus, dev, func;
    for (bus = 0; bus < 256; bus++) {
        for (dev = 0; dev < 32; dev++) {
            uint32_t id0 = pci_read32((uint8_t)bus, (uint8_t)dev, 0, 0x00);
            if ((id0 & 0xFFFF) == 0xFFFF) continue; // nothing at function 0 -> whole device absent

            uint32_t hdr = pci_read32((uint8_t)bus, (uint8_t)dev, 0, 0x0C);
            int multifunction = ((hdr >> 16) & 0x80) != 0;
            uint32_t max_func = multifunction ? 8 : 1;

            for (func = 0; func < max_func; func++) {
                uint32_t id = pci_read32((uint8_t)bus, (uint8_t)dev, (uint8_t)func, 0x00);
                if ((id & 0xFFFF) == 0xFFFF) continue;

                uint32_t classreg = pci_read32((uint8_t)bus, (uint8_t)dev, (uint8_t)func, 0x08);
                uint8_t prog_if    = (uint8_t)((classreg >> 8) & 0xFF);
                uint8_t subclass   = (uint8_t)((classreg >> 16) & 0xFF);
                uint8_t base_class = (uint8_t)((classreg >> 24) & 0xFF);

                if (base_class == 0x01 && subclass == 0x06 && prog_if == 0x01) {
                    uint32_t bar5 = pci_read32((uint8_t)bus, (uint8_t)dev, (uint8_t)func, 0x24);
                    *out_abar = bar5 & 0xFFFFFFF0u;
                    return 0;
                }
            }
        }
    }
    return -1;
}

// Port control

static int port_wait_idle(uint32_t timeout_ms) {
    uint32_t start = get_uptime_ms();
    while (port->cmd & (PORT_CMD_CR | PORT_CMD_FR)) {
        if (get_uptime_ms() - start > timeout_ms) return -1;
    }
    return 0;
}

static void port_stop(void) {
    port->cmd &= ~(uint32_t)PORT_CMD_ST;
    port->cmd &= ~(uint32_t)PORT_CMD_FRE;
    port_wait_idle(1000);
}

static void port_start(void) {
    port_wait_idle(1000);
    port->cmd |= PORT_CMD_FRE;
    port->cmd |= PORT_CMD_ST;
}

// Command issue - the core of the driver. Builds a Register H2D FIS
// in the command table, points a single PRDT entry at the caller's
// buffer (skipped entirely for non-data commands like FLUSH CACHE),
// rings the doorbell for slot 0, and polls for completion with a
// real-time-bounded timeout (never a raw iteration count - see the
// legacy ATA driver's history for exactly why that matters).

static int ahci_do_command(uint8_t ata_cmd, uint64_t lba, uint16_t count, void* buf, int is_write) {
    if (!port) return -1;

    uint32_t start = get_uptime_ms();
    while (port->tfd & (TFD_BSY | TFD_DRQ)) {
        if (get_uptime_ms() - start > 5000) return -1;
    }

    uint32_t buf_bytes = (uint32_t)count * 512;
    int has_data = (buf != 0 && buf_bytes > 0);

    cmd_list[0].dw0 = 5u /* CFL: 20-byte FIS = 5 dwords */
                     | (is_write ? (1u << 6) : 0)
                     | ((has_data ? 1u : 0u) << 16); // PRDTL
    cmd_list[0].prdbc = 0;
    cmd_list[0].ctba = (uint32_t)(uintptr_t)&cmd_table;
    cmd_list[0].ctbau = 0;

    int i;
    for (i = 0; i < 64; i++) cmd_table.cfis[i] = 0;

    if (has_data) {
        cmd_table.prdt[0].dba = (uint32_t)(uintptr_t)buf;
        cmd_table.prdt[0].dbau = 0;
        cmd_table.prdt[0].reserved = 0;
        cmd_table.prdt[0].dbc_ic = (buf_bytes - 1) & 0x3FFFFF;
    }

    uint8_t* fis = cmd_table.cfis;
    fis[0]  = 0x27; // Register FIS - Host to Device
    fis[1]  = 0x80; // C=1 (this is a command, not a control update)
    fis[2]  = ata_cmd;
    fis[3]  = 0;                              // features low
    fis[4]  = (uint8_t)(lba & 0xFF);
    fis[5]  = (uint8_t)((lba >> 8) & 0xFF);
    fis[6]  = (uint8_t)((lba >> 16) & 0xFF);
    fis[7]  = 0x40;                           // device: LBA mode
    fis[8]  = (uint8_t)((lba >> 24) & 0xFF);
    fis[9]  = (uint8_t)((lba >> 32) & 0xFF);
    fis[10] = (uint8_t)((lba >> 40) & 0xFF);
    fis[11] = 0;                              // features high
    fis[12] = (uint8_t)(count & 0xFF);
    fis[13] = (uint8_t)((count >> 8) & 0xFF);
    fis[14] = 0;
    fis[15] = 0;

    port->is = 0xFFFFFFFFu; // clear stale status before issuing
    port->ci = 1u;          // ring the doorbell for slot 0

    start = get_uptime_ms();
    while (port->ci & 1u) {
        if (port->tfd & TFD_ERR) return -1;
        if (get_uptime_ms() - start > 10000) return -1;
    }
    if (port->tfd & TFD_ERR) return -1;
    return 0;
}

// Public API

int ahci_init(void) {
    ahci_available = 0;
    port = 0;

    uint32_t abar;
    if (find_ahci_pci_device(&abar) != 0 || abar == 0) return -1;

    ghc = (hba_ghc_t*)(uintptr_t)abar;
    ghc->ghc |= GHC_AE; // some firmware leaves AHCI mode off until the OS asks for it

    uint32_t pi = ghc->pi;
    int i;
    for (i = 0; i < 32; i++) {
        if (!(pi & (1u << i))) continue;
        hba_port_t* p = (hba_port_t*)((uint8_t*)ghc + 0x100 + i * 0x80);
        if ((p->ssts & 0x0F) != 3) continue;      // DET != 3 -> no device / no PHY link
        if (p->sig != 0x00000101) continue;        // not a plain SATA drive (skip ATAPI etc)
        port = p;
        break;
    }
    if (!port) return -1;

    port_stop();

    for (i = 0; i < 32; i++) {
        cmd_list[i].dw0 = 0; cmd_list[i].prdbc = 0;
        cmd_list[i].ctba = 0; cmd_list[i].ctbau = 0;
    }
    for (i = 0; i < 256; i++) fis_recv[i] = 0;

    port->clb  = (uint32_t)(uintptr_t)cmd_list;
    port->clbu = 0;
    port->fb   = (uint32_t)(uintptr_t)fis_recv;
    port->fbu  = 0;
    port->serr = 0xFFFFFFFFu;
    port->is   = 0xFFFFFFFFu;

    port_start();

    if (ahci_do_command(0xEC /* IDENTIFY DEVICE */, 0, 1, identify_buf, 0) != 0) {
        port = 0;
        return -1;
    }

    uint64_t lba48 = ((uint64_t)identify_buf[103] << 48) | ((uint64_t)identify_buf[102] << 32)
                    | ((uint64_t)identify_buf[101] << 16) | identify_buf[100];
    uint32_t lba28 = ((uint32_t)identify_buf[61] << 16) | identify_buf[60];
    drive_sectors = lba48 ? lba48 : lba28;
    if (drive_sectors == 0) { port = 0; return -1; }

    ahci_available = 1;
    return 0;
}

int ahci_is_available(void) { return ahci_available; }

uint64_t ahci_get_sector_count(void) { return drive_sectors; }

int ahci_read_sectors(uint64_t lba, uint16_t count, void* buf) {
    if (!ahci_available) return -1;
    return ahci_do_command(0x25 /* READ DMA EXT */, lba, count, buf, 0);
}

int ahci_write_sectors(uint64_t lba, uint16_t count, const void* buf) {
    if (!ahci_available) return -1;
    if (ahci_do_command(0x35 /* WRITE DMA EXT */, lba, count, (void*)buf, 1) != 0) return -1;
    return ahci_do_command(0xEA /* FLUSH CACHE EXT */, 0, 0, 0, 0);
}

// ---- ATAPI (CD/DVD) on an AHCI port ---------------------------------------
//
// On machines whose BIOS only offers AHCI mode the optical drive is a SATA
// ATAPI device on an AHCI port and does not exist on the legacy IDE ports at
// all, so the legacy ATAPI driver never finds it. This is a read-only driver
// for it: ATA PACKET (0xA0) carrying a SCSI READ(10), data via the PRDT.
// It has its own command list / FIS area / command table, separate from the
// hard drive's, so both can be in use at once.

#define ATAPI_SIG 0xEB140101u

static hba_cmd_header_t cd_list[32] __attribute__((aligned(1024)));
static uint8_t cd_fis[256] __attribute__((aligned(256)));
static hba_cmd_table_t cd_table __attribute__((aligned(128)));
static uint8_t cd_bounce[2048] __attribute__((aligned(16)));
static uint8_t cd_small[16] __attribute__((aligned(16)));
static hba_port_t* cd_port = 0;
static uint32_t cd_nblocks = 0;
static int cd_ready = 0;

static void cd_port_stop(void) {
    uint32_t start = get_uptime_ms();
    cd_port->cmd &= ~(uint32_t)PORT_CMD_ST;
    while ((cd_port->cmd & PORT_CMD_CR) && get_uptime_ms() - start < 1000) { }
    cd_port->cmd &= ~(uint32_t)PORT_CMD_FRE;
    start = get_uptime_ms();
    while ((cd_port->cmd & PORT_CMD_FR) && get_uptime_ms() - start < 1000) { }
}

static void cd_port_start(void) {
    uint32_t start = get_uptime_ms();
    while ((cd_port->cmd & PORT_CMD_CR) && get_uptime_ms() - start < 1000) { }
    cd_port->cmd |= PORT_CMD_FRE;
    cd_port->cmd |= PORT_CMD_ST;
}

/* after an error the port has to be restarted before the next command */
static void cd_recover(void) {
    cd_port_stop();
    cd_port->serr = 0xFFFFFFFFu;
    cd_port->is = 0xFFFFFFFFu;
    cd_port_start();
}

/* One 12-byte packet command, up to `len` bytes of data into buf (len even).
 * 0 on success, -1 on error or timeout. */
static int cd_packet(const uint8_t* cdb, void* buf, uint32_t len, uint32_t timeout_ms) {
    uint32_t start = get_uptime_ms();
    uint32_t i;
    uint8_t* fis;

    while (cd_port->tfd & (TFD_BSY | TFD_DRQ)) {
        if (get_uptime_ms() - start > 3000) { cd_recover(); return -1; }
    }

    cd_list[0].dw0 = 5u | (1u << 5) /* A: ATAPI, the ACMD area holds the packet */
                   | ((len ? 1u : 0u) << 16);
    cd_list[0].prdbc = 0;
    cd_list[0].ctba = (uint32_t)(uintptr_t)&cd_table;
    cd_list[0].ctbau = 0;

    for (i = 0; i < 64; i++) cd_table.cfis[i] = 0;
    for (i = 0; i < 16; i++) cd_table.acmd[i] = i < 12 ? cdb[i] : 0;
    if (len) {
        cd_table.prdt[0].dba = (uint32_t)(uintptr_t)buf;
        cd_table.prdt[0].dbau = 0;
        cd_table.prdt[0].reserved = 0;
        cd_table.prdt[0].dbc_ic = (len - 1) & 0x3FFFFF;
    }

    fis = cd_table.cfis;
    fis[0] = 0x27;
    fis[1] = 0x80;
    fis[2] = 0xA0;                                   /* PACKET */
    fis[3] = 0;                                      /* features: PIO data phase */
    fis[5] = (uint8_t)(len > 0xFFFE ? 0xFE : (len & 0xFF));   /* byte count limit */
    fis[6] = (uint8_t)(len > 0xFFFE ? 0xFF : ((len >> 8) & 0xFF));
    fis[7] = 0;

    cd_port->is = 0xFFFFFFFFu;
    cd_port->ci = 1u;

    start = get_uptime_ms();
    while (cd_port->ci & 1u) {
        if ((cd_port->is & (1u << 30)) || (cd_port->tfd & TFD_ERR)) { cd_recover(); return -1; }
        if (get_uptime_ms() - start > timeout_ms) { cd_recover(); return -1; }
    }
    if ((cd_port->is & (1u << 30)) || (cd_port->tfd & TFD_ERR)) { cd_recover(); return -1; }
    return 0;
}

int ahci_cd_init(void) {
    uint32_t abar, start;
    hba_ghc_t* g;
    int i, tries;
    uint8_t cdb[12];

    cd_ready = 0;
    cd_port = 0;
    if (find_ahci_pci_device(&abar) != 0 || abar == 0) return -1;
    g = (hba_ghc_t*)(uintptr_t)abar;
    g->ghc |= GHC_AE;

    for (i = 0; i < 32; i++) {
        hba_port_t* p;
        if (!(g->pi & (1u << i))) continue;
        p = (hba_port_t*)((uint8_t*)g + 0x100 + i * 0x80);
        if ((p->ssts & 0x0F) != 3) continue;          /* no device / no link */
        if (p->sig != ATAPI_SIG) continue;            /* only optical drives here */
        cd_port = p;
        break;
    }
    if (!cd_port) return -1;

    cd_port_stop();
    for (i = 0; i < 32; i++) {
        cd_list[i].dw0 = 0; cd_list[i].prdbc = 0;
        cd_list[i].ctba = 0; cd_list[i].ctbau = 0;
    }
    for (i = 0; i < 256; i++) cd_fis[i] = 0;
    cd_port->clb  = (uint32_t)(uintptr_t)cd_list;
    cd_port->clbu = 0;
    cd_port->fb   = (uint32_t)(uintptr_t)cd_fis;
    cd_port->fbu  = 0;
    cd_port->serr = 0xFFFFFFFFu;
    cd_port->is   = 0xFFFFFFFFu;
    cd_port_start();

    /* wait for the drive to report a disc (TEST UNIT READY). Right after
       power-up / a disc change the first commands often fail with "unit
       attention" or "becoming ready", so those are retried (up to ~6 s); but
       an empty tray ("medium not present") is final - asking REQUEST SENSE
       why it failed lets an empty drive be skipped at once instead of
       stalling every boot. */
    for (i = 0; i < 12; i++) cdb[i] = 0;
    for (tries = 0; tries < 30; tries++) {
        uint8_t sense_key, asc;
        if (cd_packet(cdb, 0, 0, 3000) == 0) break;
        for (i = 0; i < 12; i++) cdb[i] = 0;
        cdb[0] = 0x03;                                  /* REQUEST SENSE */
        cdb[4] = 18;
        for (i = 0; i < 16; i++) cd_small[i] = 0;
        sense_key = 0; asc = 0;
        if (cd_packet(cdb, cd_small, 16, 3000) == 0) {
            sense_key = cd_small[2] & 0x0F;
            asc = cd_small[12];
        }
        if (sense_key == 0x02 && asc == 0x3A) { cd_port = 0; return -1; }   /* no disc */
        for (i = 0; i < 12; i++) cdb[i] = 0;
        start = get_uptime_ms();
        while (get_uptime_ms() - start < 200) { }
    }
    if (tries == 30) { cd_port = 0; return -1; }

    /* READ CAPACITY(10): last LBA + block length, both big-endian */
    for (i = 0; i < 12; i++) cdb[i] = 0;
    cdb[0] = 0x25;
    if (cd_packet(cdb, cd_small, 8, 3000) != 0) { cd_port = 0; return -1; }
    {
        uint32_t last = ((uint32_t)cd_small[0] << 24) | ((uint32_t)cd_small[1] << 16) |
                        ((uint32_t)cd_small[2] << 8) | cd_small[3];
        uint32_t bsz  = ((uint32_t)cd_small[4] << 24) | ((uint32_t)cd_small[5] << 16) |
                        ((uint32_t)cd_small[6] << 8) | cd_small[7];
        if (bsz != 2048) { cd_port = 0; return -1; }
        cd_nblocks = last + 1;
    }
    cd_ready = 1;
    return 0;
}

uint32_t ahci_cd_blocks(void) { return cd_ready ? cd_nblocks : 0; }

/* Read `count` 2048-byte blocks starting at block lba. 0 ok, -1 error. */
int ahci_cd_read_blocks(uint32_t lba, uint32_t count, void* buf) {
    uint8_t* out = (uint8_t*)buf;
    if (!cd_ready || !buf || count == 0) return -1;
    while (count) {
        uint8_t cdb[12];
        uint32_t n = count > 32 ? 32 : count, i, attempt;
        void* dst = out;
        int aligned = (((uintptr_t)out) & 1u) == 0;
        if (!aligned) n = 1;                              /* odd address: bounce one block */
        if (!aligned) dst = cd_bounce;
        for (i = 0; i < 12; i++) cdb[i] = 0;
        cdb[0] = 0x28;                                    /* READ(10) */
        cdb[2] = (uint8_t)(lba >> 24); cdb[3] = (uint8_t)(lba >> 16);
        cdb[4] = (uint8_t)(lba >> 8);  cdb[5] = (uint8_t)lba;
        cdb[7] = (uint8_t)(n >> 8);    cdb[8] = (uint8_t)n;
        for (attempt = 0; attempt < 3; attempt++)
            if (cd_packet(cdb, dst, n * 2048, 10000) == 0) break;
        if (attempt == 3) return -1;
        if (!aligned) for (i = 0; i < 2048; i++) out[i] = cd_bounce[i];
        out += n * 2048;
        lba += n;
        count -= n;
    }
    return 0;
}

static inline uint16_t rd16w(const uint8_t* b, int word) { return (uint16_t)(b[word * 2] | (b[word * 2 + 1] << 8)); }

// ---- additional SATA disks (read-only) ----------------------------------------
//
// ahci_init() above drives exactly one SATA disk (the one the Storefile lives
// on). Every other SATA disk on the controller is made available here, so a
// machine with several drives shows all of them as block devices. Each port
// has its own command list / FIS area / command table.

#define XP_MAX 6

struct xp {
    hba_cmd_header_t list[32] __attribute__((aligned(1024)));
    uint8_t fis[256] __attribute__((aligned(256)));
    hba_cmd_table_t table __attribute__((aligned(128)));
    hba_port_t* hp;
    uint64_t sectors;
    char model[48];
};

static struct xp xps[XP_MAX];
static uint8_t xp_ident[512] __attribute__((aligned(16)));
static uint8_t xp_bounce[128 * 512] __attribute__((aligned(16)));
static int xp_n = 0;

static void xp_stop(struct xp* x) {
    uint32_t start = get_uptime_ms();
    x->hp->cmd &= ~(uint32_t)PORT_CMD_ST;
    while ((x->hp->cmd & PORT_CMD_CR) && get_uptime_ms() - start < 1000) { }
    x->hp->cmd &= ~(uint32_t)PORT_CMD_FRE;
    start = get_uptime_ms();
    while ((x->hp->cmd & PORT_CMD_FR) && get_uptime_ms() - start < 1000) { }
}

static void xp_start(struct xp* x) {
    uint32_t start = get_uptime_ms();
    while ((x->hp->cmd & PORT_CMD_CR) && get_uptime_ms() - start < 1000) { }
    x->hp->cmd |= PORT_CMD_FRE;
    x->hp->cmd |= PORT_CMD_ST;
}

/* one read-type ATA command (IDENTIFY DEVICE / READ DMA EXT); 0 ok, -1 error */
static int xp_cmd(struct xp* x, uint8_t ata_cmd, uint64_t lba, uint32_t count, void* buf, uint32_t bytes)
{
    uint32_t start = get_uptime_ms(), i;
    uint8_t* fis;
    while (x->hp->tfd & (TFD_BSY | TFD_DRQ)) {
        if (get_uptime_ms() - start > 3000) goto fail;
    }
    x->list[0].dw0 = 5u | (1u << 16);                    /* 20-byte FIS, one PRDT entry, read */
    x->list[0].prdbc = 0;
    x->list[0].ctba = (uint32_t)(uintptr_t)&x->table;
    x->list[0].ctbau = 0;
    for (i = 0; i < 64; i++) x->table.cfis[i] = 0;
    x->table.prdt[0].dba = (uint32_t)(uintptr_t)buf;
    x->table.prdt[0].dbau = 0;
    x->table.prdt[0].reserved = 0;
    x->table.prdt[0].dbc_ic = (bytes - 1) & 0x3FFFFF;
    fis = x->table.cfis;
    fis[0] = 0x27; fis[1] = 0x80; fis[2] = ata_cmd;
    fis[4] = (uint8_t)lba;         fis[5] = (uint8_t)(lba >> 8);  fis[6] = (uint8_t)(lba >> 16);
    fis[7] = 0x40;
    fis[8] = (uint8_t)(lba >> 24); fis[9] = (uint8_t)(lba >> 32); fis[10] = (uint8_t)(lba >> 40);
    fis[12] = (uint8_t)count;      fis[13] = (uint8_t)(count >> 8);
    x->hp->is = 0xFFFFFFFFu;
    x->hp->ci = 1u;
    start = get_uptime_ms();
    while (x->hp->ci & 1u) {
        if ((x->hp->tfd & TFD_ERR) || get_uptime_ms() - start > 10000) goto fail;
    }
    if (x->hp->tfd & TFD_ERR) goto fail;
    return 0;
fail:
    xp_stop(x);                                          /* restart the port for the next command */
    x->hp->serr = 0xFFFFFFFFu;
    x->hp->is = 0xFFFFFFFFu;
    xp_start(x);
    return -1;
}

/* Find and set up every SATA disk except the one ahci_init() owns.
 * Returns how many were found. */
int ahci_extra_init(void)
{
    uint32_t abar;
    hba_ghc_t* g;
    int i;
    if (xp_n) return xp_n;
    if (find_ahci_pci_device(&abar) != 0 || abar == 0) return 0;
    g = (hba_ghc_t*)(uintptr_t)abar;
    g->ghc |= GHC_AE;

    for (i = 0; i < 32 && xp_n < XP_MAX; i++) {
        hba_port_t* p;
        struct xp* x;
        uint32_t k, w;
        if (!(g->pi & (1u << i))) continue;
        p = (hba_port_t*)((uint8_t*)g + 0x100 + i * 0x80);
        if ((p->ssts & 0x0F) != 3 || p->sig != 0x00000101u) continue;   /* a SATA disk */
        if (p == port) continue;                                        /* ahci_init()'s disk */
        x = &xps[xp_n];
        x->hp = p;
        xp_stop(x);
        for (k = 0; k < 32; k++) { x->list[k].dw0 = 0; x->list[k].prdbc = 0; x->list[k].ctba = 0; x->list[k].ctbau = 0; }
        for (k = 0; k < 256; k++) x->fis[k] = 0;
        p->clb = (uint32_t)(uintptr_t)x->list; p->clbu = 0;
        p->fb = (uint32_t)(uintptr_t)x->fis;   p->fbu = 0;
        p->serr = 0xFFFFFFFFu; p->is = 0xFFFFFFFFu;
        xp_start(x);
        if (xp_cmd(x, 0xEC, 0, 0, xp_ident, 512) != 0) continue;       /* IDENTIFY DEVICE */
        if (rd16w(xp_ident, 83) & (1u << 10))
            x->sectors = (uint64_t)rd16w(xp_ident, 100) | ((uint64_t)rd16w(xp_ident, 101) << 16) |
                         ((uint64_t)rd16w(xp_ident, 102) << 32) | ((uint64_t)rd16w(xp_ident, 103) << 48);
        else
            x->sectors = (uint64_t)rd16w(xp_ident, 60) | ((uint64_t)rd16w(xp_ident, 61) << 16);
        if (x->sectors == 0) continue;
        for (k = 0; k < 20; k++) {                                     /* model: words 27..46, byte-swapped */
            w = rd16w(xp_ident, 27 + k);
            x->model[2 * k] = (char)(w >> 8);
            x->model[2 * k + 1] = (char)(w & 0xFF);
        }
        x->model[40] = 0;
        for (k = 40; k > 0 && x->model[k - 1] == ' '; k--) x->model[k - 1] = 0;
        xp_n++;
    }
    return xp_n;
}

const char* ahci_extra_model(int i) { return (i >= 0 && i < xp_n) ? xps[i].model : ""; }
uint64_t ahci_extra_sectors(int i) { return (i >= 0 && i < xp_n) ? xps[i].sectors : 0; }

/* read `count` 512-byte sectors; 0 ok, -1 error */
int ahci_extra_read(int i, uint64_t lba, uint32_t count, void* buf)
{
    uint8_t* out = (uint8_t*)buf;
    if (i < 0 || i >= xp_n || !buf || !count) return -1;
    if (lba + count > xps[i].sectors) return -1;
    while (count) {
        uint32_t n = count > 128 ? 128 : count, k;
        int aligned = (((uintptr_t)out) & 1u) == 0;
        void* dst = aligned ? (void*)out : (void*)xp_bounce;
        int tries;
        for (tries = 0; tries < 2; tries++)
            if (xp_cmd(&xps[i], 0x25 /* READ DMA EXT */, lba, n, dst, n * 512) == 0) break;
        if (tries == 2) return -1;
        if (!aligned) for (k = 0; k < n * 512; k++) out[k] = xp_bounce[k];
        out += n * 512; lba += n; count -= n;
    }
    return 0;
}
