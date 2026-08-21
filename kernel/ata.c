#include <stdint.h>
#include "../include/ata.h"

// Self-contained port I/O helpers (kept local so this file has no
// dependency on kernel.c's globals).
static inline void outb(uint16_t port, uint8_t val) {
    asm volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outw(uint16_t port, uint16_t val) {
    asm volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    asm volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static uint16_t io_base(int channel) {
    return channel ? ATA_SECONDARY_IO : ATA_PRIMARY_IO;
}

static uint16_t ctrl_base(int channel) {
    return channel ? ATA_SECONDARY_CTRL : ATA_PRIMARY_CTRL;
}

// Reading the alternate status register throws away the result; each
// read costs ~100ns on real hardware, so four reads is the usual
// "give the drive 400ns" trick.
static void ata_io_wait(int channel) {
    uint16_t ctrl = ctrl_base(channel);
    inb(ctrl); inb(ctrl); inb(ctrl); inb(ctrl);
}

static int ata_poll_bsy(int channel) {
    uint16_t io = io_base(channel);
    uint32_t timeout = 2000000;
    while (timeout--) {
        if (!(inb(io + ATA_REG_STATUS) & ATA_STATUS_BSY))
            return 0;
    }
    return -1;
}

static int ata_poll_drq(int channel) {
    uint16_t io = io_base(channel);
    uint32_t timeout = 2000000;
    while (timeout--) {
        uint8_t status = inb(io + ATA_REG_STATUS);
        if (status & ATA_STATUS_ERR) return -1;
        if (status & ATA_STATUS_DF) return -1;
        if (status & 0x08) return 0; // DRQ
    }
    return -1;
}

void ata_init(void) {
    outb(ATA_PRIMARY_IO + ATA_REG_DRIVE_HEAD, 0xA0);
    ata_io_wait(0);
    outb(ATA_SECONDARY_IO + ATA_REG_DRIVE_HEAD, 0xA0);
    ata_io_wait(1);
}

int ata_detect_drive(int channel, int drive, ata_drive_info_t *info) {
    if (!info) return -1;
    info->present = 0;
    info->model[0] = 0;
    info->serial[0] = 0;
    info->firmware[0] = 0;
    info->sectors = 0;

    uint16_t io = io_base(channel);
    uint8_t drive_sel = drive ? 0xB0 : 0xA0;
    outb(io + ATA_REG_DRIVE_HEAD, drive_sel);
    ata_io_wait(channel);

    outb(io + ATA_REG_SECTOR_COUNT, 0);
    outb(io + ATA_REG_LBA_LOW, 0);
    outb(io + ATA_REG_LBA_MID, 0);
    outb(io + ATA_REG_LBA_HIGH, 0);
    outb(io + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);

    uint8_t status = inb(io + ATA_REG_STATUS);
    if (status == 0) return -1; // floating bus, nothing there

    if (ata_poll_bsy(channel) != 0) return -1;

    // A real ATA drive leaves LBA mid/high at 0 here. Non-zero usually
    // means ATAPI (e.g. a CD/DVD image) rather than a hard disk - we
    // don't support ATAPI, so treat it as "not usable" and keep looking.
    uint8_t mid = inb(io + ATA_REG_LBA_MID);
    uint8_t hi  = inb(io + ATA_REG_LBA_HIGH);
    if (mid != 0 || hi != 0) return -1;

    if (ata_poll_drq(channel) != 0) return -1;

    uint16_t id[256];
    int i;
    for (i = 0; i < 256; i++) id[i] = inw(io + ATA_REG_DATA);

    info->present = 1;
    info->sectors = ((uint32_t)id[61] << 16) | id[60];

    for (i = 0; i < 10; i++) {
        uint16_t w = id[ATA_ID_SERIAL + i];
        info->serial[i * 2]     = (char)(w >> 8);
        info->serial[i * 2 + 1] = (char)(w & 0xFF);
    }
    info->serial[20] = 0;

    for (i = 0; i < 4; i++) {
        uint16_t w = id[ATA_ID_FW_REV + i];
        info->firmware[i * 2]     = (char)(w >> 8);
        info->firmware[i * 2 + 1] = (char)(w & 0xFF);
    }
    info->firmware[8] = 0;

    for (i = 0; i < 20; i++) {
        uint16_t w = id[ATA_ID_PROD + i];
        info->model[i * 2]     = (char)(w >> 8);
        info->model[i * 2 + 1] = (char)(w & 0xFF);
    }
    info->model[40] = 0;

    return 0;
}

void ata_print_drive_info(ata_drive_info_t *info) {
    extern void print(const char* s);
    extern void print_dec(uint32_t n);

    if (!info || !info->present) {
        print("No drive detected\n");
        return;
    }

    print("Model: ");    print(info->model);    print("\n");
    print("Serial: ");   print(info->serial);   print("\n");
    print("Firmware: "); print(info->firmware); print("\n");
    print("Sectors: ");  print_dec(info->sectors); print("\n");
}

int ata_read_sectors(int channel, int drive, uint32_t lba, uint8_t count, void* buf) {
    if (!buf || count == 0) return -1;
    uint16_t io = io_base(channel);
    uint8_t drive_bit = drive ? 0xF0 : 0xE0;
    uint16_t* p = (uint16_t*)buf;

    outb(io + ATA_REG_DRIVE_HEAD, drive_bit | ((lba >> 24) & 0x0F));
    ata_io_wait(channel);
    outb(io + ATA_REG_SECTOR_COUNT, count);
    outb(io + ATA_REG_LBA_LOW,  (uint8_t)(lba & 0xFF));
    outb(io + ATA_REG_LBA_MID,  (uint8_t)((lba >> 8) & 0xFF));
    outb(io + ATA_REG_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFF));
    outb(io + ATA_REG_COMMAND, 0x20); // READ SECTORS (PIO, 28-bit)

    int s;
    for (s = 0; s < count; s++) {
        if (ata_poll_bsy(channel) != 0) return -1;
        if (ata_poll_drq(channel) != 0) return -1;
        int i;
        for (i = 0; i < 256; i++) *p++ = inw(io + ATA_REG_DATA);
        ata_io_wait(channel);
    }
    return 0;
}

int ata_write_sectors(int channel, int drive, uint32_t lba, uint8_t count, const void* buf) {
    if (!buf || count == 0) return -1;
    uint16_t io = io_base(channel);
    uint8_t drive_bit = drive ? 0xF0 : 0xE0;
    const uint16_t* p = (const uint16_t*)buf;

    outb(io + ATA_REG_DRIVE_HEAD, drive_bit | ((lba >> 24) & 0x0F));
    ata_io_wait(channel);
    outb(io + ATA_REG_SECTOR_COUNT, count);
    outb(io + ATA_REG_LBA_LOW,  (uint8_t)(lba & 0xFF));
    outb(io + ATA_REG_LBA_MID,  (uint8_t)((lba >> 8) & 0xFF));
    outb(io + ATA_REG_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFF));
    outb(io + ATA_REG_COMMAND, 0x30); // WRITE SECTORS (PIO, 28-bit)

    int s;
    for (s = 0; s < count; s++) {
        if (ata_poll_bsy(channel) != 0) return -1;
        if (ata_poll_drq(channel) != 0) return -1;
        int i;
        for (i = 0; i < 256; i++) outw(io + ATA_REG_DATA, *p++);
        ata_io_wait(channel);
    }

    // FLUSH CACHE so the write actually lands before we consider it done.
    outb(io + ATA_REG_COMMAND, 0xE7);
    ata_poll_bsy(channel);
    return 0;
}
