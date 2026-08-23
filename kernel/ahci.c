// ============================================================
// AHCI (SATA) driver
//
// Legacy PATA/IDE controllers are increasingly absent from modern
// hardware entirely - the BIOS/UEFI only offers AHCI, with no
// "Legacy"/"Compatibility" fallback. This driver talks to the real
// AHCI controller directly: PCI enumeration to find it, memory-mapped
// register access (no paging in this kernel, so MMIO is just a plain
// pointer dereference - no page tables to set up), and DMA-based
// LBA48 read/write via command lists and PRDT descriptors.
//
// This is polling-based (no AHCI interrupts), matching the style of
// the legacy ATA driver, and only uses a single command slot (slot 0)
// since we only ever need one command outstanding at a time.
// ============================================================

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

// ------------------------------------------------------------
// PCI config space access (legacy I/O-port mechanism, universally
// supported since well before AHCI existed)
// ------------------------------------------------------------

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

static uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    uint32_t address = 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11)
                      | ((uint32_t)func << 8) | (offset & 0xFC);
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

// ------------------------------------------------------------
// AHCI register layout (AHCI 1.3.1 spec)
// ------------------------------------------------------------

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

// ------------------------------------------------------------
// PCI enumeration
// ------------------------------------------------------------

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

// ------------------------------------------------------------
// Port control
// ------------------------------------------------------------

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

// ------------------------------------------------------------
// Command issue - the core of the driver. Builds a Register H2D FIS
// in the command table, points a single PRDT entry at the caller's
// buffer (skipped entirely for non-data commands like FLUSH CACHE),
// rings the doorbell for slot 0, and polls for completion with a
// real-time-bounded timeout (never a raw iteration count - see the
// legacy ATA driver's history for exactly why that matters).
// ------------------------------------------------------------

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

// ------------------------------------------------------------
// Public API
// ------------------------------------------------------------

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
