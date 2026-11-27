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

// Read-only access to an optical drive (ATAPI) on an AHCI port, for machines
// whose BIOS only offers AHCI mode (the drive is not on the legacy IDE ports
// then). ahci_cd_init() returns 0 when a drive with a disc in it was found;
// blocks are 2048 bytes. Independent of the hard drive above.
int ahci_cd_init(void);
uint32_t ahci_cd_blocks(void);
int ahci_cd_read_blocks(uint32_t lba, uint32_t count, void* buf);

// Every SATA disk other than the one ahci_init() drives, read-only, so that
// all drives of a machine are visible. ahci_extra_init() returns how many.
int ahci_extra_init(void);
const char* ahci_extra_model(int i);
uint64_t ahci_extra_sectors(int i);
int ahci_extra_read(int i, uint64_t lba, uint32_t count, void* buf);

#endif
