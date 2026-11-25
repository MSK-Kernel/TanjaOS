#include "../include/vfs.h"
#include "../include/ata.h"
#include "../include/ahci.h"

/* TanjaOS VFS core
 * Mount table, block-device registry, and path resolution.
 */

#define MAX_FSTYPES 8

static struct vfs_mount mounts[VFS_MAX_MOUNTS];
static int mount_count = 0;

static struct vfs_bdev bdevs[VFS_MAX_BDEVS];
static int bdev_count = 0;
/* store_init() probes the drives early (to learn which disk was booted from)
 * and fs_init() later runs vfs_init() + vfs_probe_drives() again: once the
 * drives have been probed, the device list is kept and not probed twice. */
static int drives_probed = 0;

static const struct vfs_ops* fstypes[MAX_FSTYPES];
static int fstype_count = 0;

static int path_is_prefix(const char* mount, const char* path) {
    int i = 0;
    while (mount[i] && path[i] && mount[i] == path[i]) i++;
    if (mount[i] != 0) return 0;
    if (path[i] == 0) return 1;
    if (mount[0] == '/' && mount[1] == 0) return 1;
    return path[i] == '/';
}

static int my_strcmp(const char* a, const char* b) {
    if (!a || !b) return 1;
    while (*a && *b) {
        if (*a != *b) return (*a > *b) ? 1 : -1;
        a++; b++;
    }
    return (*a == *b) ? 0 : (*a ? 1 : -1);
}

static void my_strcpy(char* d, const char* s, int max) {
    int i = 0;
    if (!d || !s) return;
    while (s[i] && i < max - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

void vfs_init(void) {
    int i;
    for (i = 0; i < VFS_MAX_MOUNTS; i++) {
        mounts[i].used = 0;
        mounts[i].mountpoint[0] = 0;
        mounts[i].ops = 0;
        mounts[i].sb = 0;
        mounts[i].dev = 0;
        mounts[i].fstype[0] = 0;
    }
    mount_count = 0;
    if (!drives_probed) bdev_count = 0;
    fstype_count = 0;
}

int vfs_register_fstype(const struct vfs_ops* ops) {
    if (!ops || !ops->name || fstype_count >= MAX_FSTYPES) return VFS_EINVAL;
    fstypes[fstype_count++] = ops;
    return VFS_OK;
}

const struct vfs_ops* vfs_find_fstype(const char* name) {
    int i;
    if (!name) return 0;
    for (i = 0; i < fstype_count; i++)
        if (my_strcmp(fstypes[i]->name, name) == 0)
            return fstypes[i];
    return 0;
}

int vfs_mount_root(const struct vfs_ops* ops, void* sb) {
    if (mount_count > 0) return VFS_EEXIST;
    mounts[0].used = 1;
    mounts[0].ops = ops;
    mounts[0].sb = sb;
    mounts[0].dev = 0;
    mounts[0].mountpoint[0] = '/';
    mounts[0].mountpoint[1] = 0;
    my_strcpy(mounts[0].fstype, ops && ops->name ? ops->name : "ramfs", 32);
    mount_count = 1;
    if (ops) vfs_register_fstype(ops);
    return VFS_OK;
}

int vfs_replace_root_ops(const struct vfs_ops* ops, void* sb) {
    if (mount_count < 1 || !mounts[0].used || !ops) return VFS_EINVAL;
    mounts[0].ops = ops;
    mounts[0].sb = sb;
    my_strcpy(mounts[0].fstype, ops->name ? ops->name : "root", 32);
    return VFS_OK;
}

int vfs_mount(const char* mountpoint, const char* fstype,
              struct vfs_bdev* dev, const char* opts) {
    if (!mountpoint || !fstype || mount_count >= VFS_MAX_MOUNTS) return VFS_EINVAL;
    if (mountpoint[0] != '/') return VFS_EINVAL;

    const struct vfs_ops* ops = vfs_find_fstype(fstype);
    if (!ops) return VFS_EINVAL;

    int i;
    for (i = 0; i < mount_count; i++)
        if (mounts[i].used && my_strcmp(mounts[i].mountpoint, mountpoint) == 0)
            return VFS_EEXIST;

    /* Call the filesystem's own mount hook if it has one */
    void* sb = 0;
    if (ops->mount) {
        int rc = ops->mount(&sb, dev, opts);
        if (rc != 0) return rc;
    }

    mounts[mount_count].used = 1;
    mounts[mount_count].ops = ops;
    mounts[mount_count].sb = sb;
    mounts[mount_count].dev = dev;
    my_strcpy(mounts[mount_count].mountpoint, mountpoint, VFS_PATH_MAX);
    my_strcpy(mounts[mount_count].fstype, fstype, 32);
    mount_count++;
    return VFS_OK;
}

int vfs_unmount(const char* mountpoint) {
    int i;
    for (i = 1; i < mount_count; i++) {          /* never unmount root */
        if (mounts[i].used && my_strcmp(mounts[i].mountpoint, mountpoint) == 0) {
            if (mounts[i].ops && mounts[i].ops->unmount)
                mounts[i].ops->unmount(mounts[i].sb);
            mounts[i].used = 0;
            int j;
            for (j = i; j < mount_count - 1; j++)
                mounts[j] = mounts[j + 1];
            mount_count--;
            return VFS_OK;
        }
    }
    return VFS_ENOENT;
}

struct vfs_mount* vfs_resolve(const char* abs_path, char* rel_buf) {
    if (!abs_path || !rel_buf) return 0;

    struct vfs_mount* best = 0;
    int best_len = -1;
    int i;
    for (i = 0; i < mount_count; i++) {
        if (!mounts[i].used) continue;
        if (path_is_prefix(mounts[i].mountpoint, abs_path)) {
            int len = 0;
            while (mounts[i].mountpoint[len]) len++;
            if (len > best_len) {
                best_len = len;
                best = &mounts[i];
            }
        }
    }
    if (!best) return 0;

    const char* p = abs_path;
    int mlen = 0;
    while (best->mountpoint[mlen]) mlen++;

    if (mlen == 1 && best->mountpoint[0] == '/') {
        int k = 0;
        while (p[k] && k < VFS_PATH_MAX - 1) { rel_buf[k] = p[k]; k++; }
        rel_buf[k] = 0;
        if (k == 0) { rel_buf[0] = '/'; rel_buf[1] = 0; }
    } else {
        p += mlen;
        if (*p == 0) {
            rel_buf[0] = '/'; rel_buf[1] = 0;
        } else {
            int k = 0;
            while (p[k] && k < VFS_PATH_MAX - 1) { rel_buf[k] = p[k]; k++; }
            rel_buf[k] = 0;
        }
    }
    return best;
}

int vfs_list_mounts(char* buf, uint32_t* size) {
    if (!buf || !size) return VFS_EINVAL;
    int pos = 0;
    int i;
    for (i = 0; i < mount_count && pos < 4000; i++) {
        if (!mounts[i].used) continue;
        const char* mp = mounts[i].mountpoint;
        const char* ft = mounts[i].fstype;
        const char* dn = mounts[i].dev ? mounts[i].dev->name : "-";
        /* "mountpoint fstype device\n" */
        int k;
        for (k = 0; mp[k] && pos < 4090; k++) buf[pos++] = mp[k];
        buf[pos++] = ' ';
        for (k = 0; ft[k] && pos < 4090; k++) buf[pos++] = ft[k];
        buf[pos++] = ' ';
        for (k = 0; dn[k] && pos < 4090; k++) buf[pos++] = dn[k];
        buf[pos++] = '\n';
    }
    buf[pos] = 0;
    *size = pos;
    return VFS_OK;
}


int vfs_register_bdev(struct vfs_bdev* dev) {
    if (!dev || bdev_count >= VFS_MAX_BDEVS) return VFS_EINVAL;
    bdevs[bdev_count] = *dev;
    bdev_count++;
    return VFS_OK;
}

struct vfs_bdev* vfs_find_bdev(const char* name) {
    int i;
    if (!name) return 0;
    for (i = 0; i < bdev_count; i++)
        if (my_strcmp(bdevs[i].name, name) == 0)
            return &bdevs[i];
    return 0;
}

int vfs_list_bdevs(char* buf, uint32_t* size) {
    if (!buf || !size) return VFS_EINVAL;
    int pos = 0;
    int i;
    for (i = 0; i < bdev_count && pos < 4000; i++) {
        /* "name model sectors\n" */
        int k;
        for (k = 0; bdevs[i].name[k] && pos < 4090; k++) buf[pos++] = bdevs[i].name[k];
        buf[pos++] = ' ';
        for (k = 0; bdevs[i].model[k] && pos < 4090; k++) buf[pos++] = bdevs[i].model[k];
        buf[pos++] = ' ';
        /* sectors as decimal */
        char tmp[24];
        uint64_t s = bdevs[i].sectors;
        int n = 0;
        if (s == 0) { tmp[n++] = '0'; }
        else {
            char rev[24];
            int r = 0;
            while (s) { rev[r++] = '0' + (s % 10); s /= 10; }
            while (r) tmp[n++] = rev[--r];
        }
        tmp[n] = 0;
        for (k = 0; k < n && pos < 4090; k++) buf[pos++] = tmp[k];
        buf[pos++] = '\n';
    }
    buf[pos] = 0;
    *size = pos;
    return VFS_OK;
}

int vfs_bdev_count(void) {
    return bdev_count;
}

struct vfs_bdev* vfs_bdev_at(int index) {
    if (index < 0 || index >= bdev_count) return 0;
    return &bdevs[index];
}

/* ATA read/write adapters */
static int ata_bdev_read(struct vfs_bdev* dev, uint64_t lba, uint32_t count, void* buf) {
    int channel = (int)(uintptr_t)dev->private >> 8;
    int drive   = (int)(uintptr_t)dev->private & 0xff;
    return ata_read_sectors(channel, drive, (uint32_t)lba, (uint8_t)count, buf);
}
static int ata_bdev_write(struct vfs_bdev* dev, uint64_t lba, uint32_t count, const void* buf) {
    int channel = (int)(uintptr_t)dev->private >> 8;
    int drive   = (int)(uintptr_t)dev->private & 0xff;
    return ata_write_sectors(channel, drive, (uint32_t)lba, (uint8_t)count, buf);
}

static int ahci_bdev_read(struct vfs_bdev* dev, uint64_t lba, uint32_t count, void* buf) {
    (void)dev;
    return ahci_read_sectors(lba, (uint16_t)count, buf);
}
static int ahci_bdev_write(struct vfs_bdev* dev, uint64_t lba, uint32_t count, const void* buf) {
    (void)dev;
    return ahci_write_sectors(lba, (uint16_t)count, buf);
}

/* ATAPI (CD/DVD) adapter. The block-device contract is 512-byte sectors;
 * the drive speaks 2048-byte blocks, so unaligned requests go through a
 * one-block bounce buffer. Optical media is read-only: no write hook. */
static int atapi_bdev_read(struct vfs_bdev* dev, uint64_t lba, uint32_t count, void* buf) {
    int channel = (int)(uintptr_t)dev->private >> 8;
    int drive   = (int)(uintptr_t)dev->private & 0xff;
    uint8_t* out = (uint8_t*)buf;
    static uint8_t bounce[2048];

    if (!buf || count == 0) return -1;
    if ((lba & 3) == 0 && (count & 3) == 0)
        return atapi_read_blocks(channel, drive, (uint32_t)(lba >> 2), count >> 2, buf);

    while (count) {
        uint32_t blk = (uint32_t)(lba >> 2);
        uint32_t sec = (uint32_t)(lba & 3);
        uint32_t n = 4 - sec;
        uint32_t i;
        if (n > count) n = count;
        if (atapi_read_blocks(channel, drive, blk, 1, bounce) != 0) return -1;
        for (i = 0; i < n * 512; i++) out[i] = bounce[sec * 512 + i];
        out += n * 512;
        lba += n;
        count -= n;
    }
    return 0;
}

/* Same, for an optical drive on an AHCI port. */
static int ahci_cd_bdev_read(struct vfs_bdev* dev, uint64_t lba, uint32_t count, void* buf) {
    uint8_t* out = (uint8_t*)buf;
    static uint8_t bounce[2048];
    (void)dev;
    if (!buf || count == 0) return -1;
    if ((lba & 3) == 0 && (count & 3) == 0)
        return ahci_cd_read_blocks((uint32_t)(lba >> 2), count >> 2, buf);
    while (count) {
        uint32_t blk = (uint32_t)(lba >> 2);
        uint32_t sec = (uint32_t)(lba & 3);
        uint32_t n = 4 - sec;
        uint32_t i;
        if (n > count) n = count;
        if (ahci_cd_read_blocks(blk, 1, bounce) != 0) return -1;
        for (i = 0; i < n * 512; i++) out[i] = bounce[sec * 512 + i];
        out += n * 512;
        lba += n;
        count -= n;
    }
    return 0;
}

void vfs_probe_drives(void) {
    if (drives_probed) return;
    drives_probed = 1;
    /* ATA primary master / slave */
    ata_drive_info_t info;
    int ch, dr;
    int ata_slot[4] = {0, 0, 0, 0};
    int sr_count = 0;
    for (ch = 0; ch < 2; ch++) {
        for (dr = 0; dr < 2; dr++) {
            if (ata_detect_drive(ch, dr, &info) == 0 && info.present) {
                ata_slot[ch * 2 + dr] = 1;
                struct vfs_bdev d;
                d.name[0] = 'a'; d.name[1] = 't'; d.name[2] = 'a';
                d.name[3] = '0' + (ch * 2 + dr);
                d.name[4] = 0;
                my_strcpy(d.model, info.model, 48);
                d.sectors = info.sectors;
                d.present = 1;
                d.removable = info.removable;
                d.read = ata_bdev_read;
                d.write = ata_bdev_write;
                d.private = (void*)(uintptr_t)((ch << 8) | dr);
                vfs_register_bdev(&d);
            }
        }
    }

    /* ATAPI CD/DVD drives on the legacy IDE channels: sr0, sr1, ...
     * Only slots that did not answer as a plain ATA disk are tried. */
    for (ch = 0; ch < 2; ch++) {
        for (dr = 0; dr < 2; dr++) {
            if (ata_slot[ch * 2 + dr]) continue;
            if (sr_count < 4 && atapi_detect_drive(ch, dr, &info) == 0 && info.present) {
                struct vfs_bdev d;
                d.name[0] = 's'; d.name[1] = 'r';
                d.name[2] = '0' + sr_count; d.name[3] = 0;
                my_strcpy(d.model, info.model, 48);
                d.sectors = info.sectors;
                d.present = 1;
                d.removable = 1;
                d.read = atapi_bdev_read;
                d.write = 0;
                d.private = (void*)(uintptr_t)((ch << 8) | dr);
                vfs_register_bdev(&d);
                sr_count++;
            }
        }
    }

    /* Optical drive on an AHCI port (BIOS in AHCI mode): sr<N> as well. */
    if (sr_count < 4 && ahci_cd_init() == 0) {
        struct vfs_bdev d;
        d.name[0] = 's'; d.name[1] = 'r';
        d.name[2] = '0' + sr_count; d.name[3] = 0;
        my_strcpy(d.model, "AHCI CD/DVD", 48);
        d.sectors = (uint64_t)ahci_cd_blocks() * 4;
        d.present = 1;
        d.removable = 1;
        d.read = ahci_cd_bdev_read;
        d.write = 0;
        d.private = 0;
        vfs_register_bdev(&d);
        sr_count++;
    }

    /* AHCI (single selected drive) */
    if (ahci_is_available()) {
        struct vfs_bdev d;
        my_strcpy(d.name, "ahci0", 32);
        my_strcpy(d.model, "AHCI SATA", 48);
        d.sectors = ahci_get_sector_count();
        d.present = 1;
        d.removable = 0;
        d.read = ahci_bdev_read;
        d.write = ahci_bdev_write;
        d.private = 0;
        vfs_register_bdev(&d);
    }
}
