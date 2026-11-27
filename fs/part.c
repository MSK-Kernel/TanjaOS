/* Partition tables (MBR with extended/logical partitions, and GPT).
 *
 * Every partition of every disk becomes a block device of its own, named
 * after the disk: ata0 -> ata0p1, ata0p2, ...; ahci0 -> ahci0p1, ... so that
 * "mount /dev/ata0p1 /mnt" works. Optical drives are not scanned. */
#include <stdint.h>
#include "include/vfs.h"
#include "include/blkio.h"

#define MAX_PARTS 24

struct part {
    struct vfs_bdev* parent;
    uint64_t start;
};

static struct part parts[MAX_PARTS];
static int nparts = 0;

static int part_read(struct vfs_bdev* dev, uint64_t lba, uint32_t count, void* buf) {
    struct part* p = (struct part*)dev->private;
    if (!p || lba + count > dev->sectors) return -1;
    return p->parent->read(p->parent, p->start + lba, count, buf);
}

static int part_write(struct vfs_bdev* dev, uint64_t lba, uint32_t count, const void* buf) {
    struct part* p = (struct part*)dev->private;
    if (!p || !p->parent->write || lba + count > dev->sectors) return -1;
    return p->parent->write(p->parent, p->start + lba, count, buf);
}

static void add_part(struct vfs_bdev* parent, int number, uint64_t start, uint64_t len)
{
    struct vfs_bdev d;
    char* z = (char*)&d;
    uint32_t k, n = 0;
    if (nparts >= MAX_PARTS || len == 0) return;
    if (start + len > parent->sectors) return;                 /* does not fit on the disk */
    for (k = 0; k < sizeof d; k++) z[k] = 0;
    for (k = 0; parent->name[k] && n < 24; k++) d.name[n++] = parent->name[k];
    d.name[n++] = 'p';
    if (number >= 10) d.name[n++] = (char)('0' + number / 10);
    d.name[n++] = (char)('0' + number % 10);
    d.name[n] = 0;
    {
        const char* pre = "partition of ";
        n = 0;
        for (k = 0; pre[k]; k++) d.model[n++] = pre[k];
        for (k = 0; parent->name[k] && n < 46; k++) d.model[n++] = parent->name[k];
        d.model[n] = 0;
    }
    d.sectors = len;
    d.present = 1;
    d.removable = parent->removable;
    d.read = part_read;
    d.write = parent->write ? part_write : 0;
    parts[nparts].parent = parent;
    parts[nparts].start = start;
    d.private = &parts[nparts];
    nparts++;
    vfs_register_bdev(&d);
}

static int is_extended(uint8_t t) { return t == 0x05 || t == 0x0F || t == 0x85; }

/* GPT: header at LBA 1, entries wherever it says. Returns 1 if a GPT was found. */
static int scan_gpt(struct vfs_bdev* d)
{
    static uint8_t hdr[512];
    static uint8_t ent[512 * 4];
    uint64_t elba;
    uint32_t count, esz, i, per, number = 0;

    if (d->read(d, 1, 1, hdr) != 0) return 0;
    if (!(hdr[0] == 'E' && hdr[1] == 'F' && hdr[2] == 'I' && hdr[3] == ' ' &&
          hdr[4] == 'P' && hdr[5] == 'A' && hdr[6] == 'R' && hdr[7] == 'T'))
        return 0;
    elba = rd64(hdr + 72);
    count = rd32(hdr + 80);
    esz = rd32(hdr + 84);
    if (esz < 128 || esz > 512 || (esz & (esz - 1)) != 0 || count == 0) return 0;
    if (count > 128) count = 128;
    per = (512 * 4) / esz;                                   /* entries per 4-sector read */
    for (i = 0; i < count; i += per) {
        uint32_t j, n = count - i < per ? count - i : per;
        if (d->read(d, elba + (uint64_t)i * esz / 512, 4, ent) != 0) return 1;
        for (j = 0; j < n; j++) {
            const uint8_t* e = ent + j * esz;
            uint32_t b, used = 0;
            uint64_t first, last;
            for (b = 0; b < 16; b++) if (e[b]) used = 1;     /* type GUID all zero = unused slot */
            number++;
            if (!used) continue;
            first = rd64(e + 32);
            last = rd64(e + 40);
            if (last < first) continue;
            add_part(d, (int)(i + j + 1), first, last - first + 1);
        }
    }
    return 1;
}

static void scan_mbr(struct vfs_bdev* d, const uint8_t* mbr)
{
    int i;
    for (i = 0; i < 4; i++) {
        const uint8_t* e = mbr + 446 + 16 * i;
        uint8_t type = e[4];
        uint32_t start = rd32(e + 8), len = rd32(e + 12);
        if (type == 0 || type == 0xEE || len == 0) continue;
        if (is_extended(type)) {
            /* chain of EBRs: each holds one logical partition and a link to the next */
            static uint8_t ebr[512];
            uint32_t ext_start = start, cur = start;
            int number = 5, guard;
            for (guard = 0; guard < 32; guard++) {
                const uint8_t* l;
                const uint8_t* nx;
                if (d->read(d, cur, 1, ebr) != 0 || ebr[510] != 0x55 || ebr[511] != 0xAA) break;
                l = ebr + 446;
                nx = ebr + 446 + 16;
                if (l[4] != 0 && rd32(l + 12))
                    add_part(d, number++, (uint64_t)cur + rd32(l + 8), rd32(l + 12));
                if (!is_extended(nx[4]) || rd32(nx + 8) == 0) break;
                cur = ext_start + rd32(nx + 8);
            }
            continue;
        }
        add_part(d, i + 1, start, len);
    }
}

void vfs_scan_partitions(void)
{
    static uint8_t mbr[512];
    int n = vfs_bdev_count(), i;
    for (i = 0; i < n; i++) {
        struct vfs_bdev* d = vfs_bdev_at(i);
        int has_ee = 0, k;
        if (!d || !d->present || !d->read || d->sectors < 64) continue;
        if (d->name[0] == 's' && d->name[1] == 'r') continue;            /* optical */
        if (rfs_len(d->name) > 10 && d->name[rfs_len(d->name) - 3] == 'p') continue;
        if (d->read(d, 0, 1, mbr) != 0 || mbr[510] != 0x55 || mbr[511] != 0xAA) continue;
        for (k = 0; k < 4; k++) if (mbr[446 + 16 * k + 4] == 0xEE) has_ee = 1;
        if (has_ee && scan_gpt(d)) continue;
        scan_mbr(d, mbr);
    }
}
