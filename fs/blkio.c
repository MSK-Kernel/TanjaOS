#include <stdint.h>
#include "include/blkio.h"
#include "include/vfs.h"

#define CHUNK_SECTORS 128u        /* per device call when reading straight into the caller's buffer */
#define BOUNCE_SECTORS 8u

int blk_read(struct vfs_bdev* d, uint64_t off, void* buf, uint32_t len)
{
    static uint8_t bounce[BOUNCE_SECTORS * 512] __attribute__((aligned(16)));
    uint8_t* out = (uint8_t*)buf;

    if (!d || !d->read || (!buf && len)) return -1;
    if (d->sectors && off + len > d->sectors * 512ull) return -1;

    while (len) {
        uint64_t lba = off >> 9;
        uint32_t head = (uint32_t)(off & 511u);
        if (head == 0 && len >= 512 && (((uintptr_t)out) & 3u) == 0) {
            uint32_t n = len >> 9;
            if (n > CHUNK_SECTORS) n = CHUNK_SECTORS;
            if (d->read(d, lba, n, out) != 0) return -1;
            out += n * 512; off += n * 512; len -= n * 512;
        } else {
            uint32_t n = (head + len + 511u) >> 9, take, i;
            if (n > BOUNCE_SECTORS) n = BOUNCE_SECTORS;
            if (d->read(d, lba, n, bounce) != 0) return -1;
            take = n * 512 - head;
            if (take > len) take = len;
            for (i = 0; i < take; i++) out[i] = bounce[head + i];
            out += take; off += take; len -= take;
        }
    }
    return 0;
}
