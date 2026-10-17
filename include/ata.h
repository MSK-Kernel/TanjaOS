#ifndef ATA_H
#define ATA_H

#include <stdint.h>

// ATA I/O Ports
#define ATA_PRIMARY_IO       0x1F0
#define ATA_PRIMARY_CTRL     0x3F6
#define ATA_SECONDARY_IO     0x170
#define ATA_SECONDARY_CTRL   0x376

// ATA Registers (offsets from base)
#define ATA_REG_DATA         0x00
#define ATA_REG_ERROR        0x01
#define ATA_REG_SECTOR_COUNT 0x02
#define ATA_REG_LBA_LOW      0x03
#define ATA_REG_LBA_MID      0x04
#define ATA_REG_LBA_HIGH     0x05
#define ATA_REG_DRIVE_HEAD   0x06
#define ATA_REG_STATUS       0x07
#define ATA_REG_COMMAND      0x07
#define ATA_REG_ALT_STATUS   0x06   // control port

// Status Register Bits
#define ATA_STATUS_BSY       0x80
#define ATA_STATUS_DRDY      0x40
#define ATA_STATUS_DF        0x20
#define ATA_STATUS_ERR       0x01

// Command Codes
#define ATA_CMD_IDENTIFY     0xEC

// IDENTIFY Data Offsets
#define ATA_ID_PROD          27      // Model name (20 words = 40 bytes)
#define ATA_ID_SERIAL        10      // Serial number (10 words = 20 bytes)
#define ATA_ID_FW_REV        23      // Firmware revision (4 words = 8 bytes)

// Structure for disk info
typedef struct {
    char model[41];     // 40 chars + null terminator
    char serial[21];    // 20 chars + null terminator
    char firmware[9];   // 8 chars + null terminator
    uint32_t sectors;   // Total sectors (if LBA28 supported)
    int present;        // 1 if drive exists
} ata_drive_info_t;

// Function prototypes
void ata_init(void);

// channel: 0 = primary (0x1F0), 1 = secondary (0x170)
// drive:   0 = master, 1 = slave
int ata_detect_drive(int channel, int drive, ata_drive_info_t *info);
void ata_print_drive_info(ata_drive_info_t *info);

// Raw PIO sector I/O, LBA28. count is in 512-byte sectors (1-255).
// Returns 0 on success, -1 on error/timeout.
int ata_read_sectors(int channel, int drive, uint32_t lba, uint8_t count, void* buf);
int ata_write_sectors(int channel, int drive, uint32_t lba, uint8_t count, const void* buf);

#endif
