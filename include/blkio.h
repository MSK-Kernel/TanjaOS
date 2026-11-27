#ifndef BLKIO_H
#define BLKIO_H

#include <stdint.h>
#include "vfs.h"

/* Byte-granular reads from a block device (512-byte sectors underneath), for
 * the on-disk filesystem drivers (fatfs, extfs, btrfs). 0 = ok, -1 = error
 * (I/O error, or reading past the end of the device). */
int blk_read(struct vfs_bdev* d, uint64_t off, void* buf, uint32_t len);

/* Partition tables: after the drives are probed, register every MBR / GPT
 * partition of every disk as its own block device ("ata0p1", "ahci0p2", ...).
 * Called once from vfs_probe_drives(). */
void vfs_scan_partitions(void);

/* ---- tiny helpers shared by the filesystem drivers (no libc here) ---- */

static inline uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t rd64(const uint8_t* p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }

static inline void rfs_copy(void* dst, const void* src, uint32_t n) {
    uint8_t* d = (uint8_t*)dst; const uint8_t* s = (const uint8_t*)src;
    while (n--) *d++ = *s++;
}
static inline void rfs_zero(void* dst, uint32_t n) {
    uint8_t* d = (uint8_t*)dst;
    while (n--) *d++ = 0;
}
static inline uint32_t rfs_len(const char* s) { uint32_t n = 0; while (s[n]) n++; return n; }

#endif
