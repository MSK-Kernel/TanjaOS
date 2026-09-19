#ifndef AHCI_H
#define AHCI_H

#include <stdint.h>

// Finds a PCI AHCI controller (class 0x01/subclass 0x06/progif 0x01),
// picks the first port with a present plain SATA drive (SATA drives
// only - not ATAPI), and initializes it. Returns 0 on success, -1 if
// no AHCI controller or no usable drive was found. This is what makes
// persistence work on hardware where the BIOS only offers AHCI mode
// (no legacy IDE compatibility option) - increasingly the only option
// on modern machines.
int ahci_init(void);

// True once ahci_init() has found and initialized a usable port.
int ahci_is_available(void);

// LBA48 sector read/write on the drive ahci_init() selected. count is
// in 512-byte sectors. Returns 0 on success, -1 on error.
int ahci_read_sectors(uint64_t lba, uint16_t count, void* buf);
int ahci_write_sectors(uint64_t lba, uint16_t count, const void* buf);

// Total sector count of the selected drive (from IDENTIFY DEVICE,
// LBA48-aware), 0 if no drive is available.
uint64_t ahci_get_sector_count(void);

#endif
