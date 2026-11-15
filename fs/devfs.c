#include "../include/vfs.h"

/* TanjaOS devfs
 *
 * The filesystem mounted at /dev. Every block device the VFS has
 * registered (ata0..ata3, ahci0) shows up as a node:
 *
 *     /dev/ata0   /dev/ahci0   ...
 *
 * The nodes are generated from the block-device registry on every call,
 * so there is nothing to keep in sync: a device is in /dev exactly when
 * it is registered with the VFS. lsblk lists /dev, and `mount` accepts
 * either "ata0" or "/dev/ata0".
 *
 * Nodes are root:root 0660 and read-only metadata for now: size, mode
 * and owner work, raw sector read/write through the file API does not
 * (those ops are left NULL, so fs_read_file() etc. fail cleanly).
 */

#define DEVFS_NODE_MODE 0660
#define DEVFS_DIR_MODE  0755

/* "/ata0" -> registered device "ata0", or NULL. "/" and anything with a
 * second slash in it are not device nodes. */
static struct vfs_bdev* dev_lookup(const char* path) {
    int i;
    struct vfs_bdev* d;
    if (!path || path[0] != '/' || path[1] == 0) return 0;
    for (i = 1; path[i]; i++)
        if (path[i] == '/') return 0;
    d = vfs_find_bdev(path + 1);
    return (d && d->present) ? d : 0;
}

static int path_is_root(const char* path) {
    return path && (path[0] == 0 || (path[0] == '/' && path[1] == 0));
}

static int devfs_dir_exists(void* sb, const char* path) {
    (void)sb;
    return path_is_root(path);
}

static int devfs_file_exists(void* sb, const char* path) {
    (void)sb;
    return dev_lookup(path) != 0;
}

static int devfs_dir_is_empty(void* sb, const char* path) {
    int i, n = vfs_bdev_count();
    (void)sb;
    if (!path_is_root(path)) return 0;
    for (i = 0; i < n; i++) {
        struct vfs_bdev* d = vfs_bdev_at(i);
        if (d && d->present) return 0;
    }
    return 1;
}

static int devfs_list_dir(void* sb, const char* path, char* buf, uint32_t* size) {
    int i, n = vfs_bdev_count();
    uint32_t pos = 0;
    (void)sb;
    if (!buf || !size) return -1;
    *size = 0;
    buf[0] = 0;
    if (!path_is_root(path)) return -1;

    for (i = 0; i < n; i++) {
        struct vfs_bdev* d = vfs_bdev_at(i);
        int k;
        if (!d || !d->present || !d->name[0]) continue;
        if (pos) buf[pos++] = '\n';
        for (k = 0; d->name[k] && k < 31 && pos < 4000; k++)
            buf[pos++] = d->name[k];
        buf[pos] = 0;
    }
    *size = pos;
    return 0;
}

/* Size of the node in bytes. The file API is 32-bit, so anything of
 * 4 GiB or more saturates; lsblk gets the exact sector count from the
 * block device itself. */
static uint32_t devfs_get_file_size(void* sb, const char* path) {
    struct vfs_bdev* d = dev_lookup(path);
    (void)sb;
    if (!d) return 0;
    if (d->sectors >= (0xFFFFFFFFull / 512ull)) return 0xFFFFFFFFu;
    return (uint32_t)(d->sectors * 512ull);
}

static int devfs_get_mode(void* sb, const char* path) {
    (void)sb;
    if (path_is_root(path)) return DEVFS_DIR_MODE;
    if (dev_lookup(path)) return DEVFS_NODE_MODE;
    return -1;
}

static int devfs_get_owner(void* sb, const char* path) {
    (void)sb;
    if (path_is_root(path) || dev_lookup(path)) return 0;   /* root */
    return -1;
}

static const struct vfs_ops devfs_ops = {
    .name = "devfs",
    .get_file_size = devfs_get_file_size,
    .file_exists = devfs_file_exists,
    .dir_exists = devfs_dir_exists,
    .dir_is_empty = devfs_dir_is_empty,
    .list_dir = devfs_list_dir,
    .get_owner = devfs_get_owner,
    .get_mode = devfs_get_mode,
};

const struct vfs_ops* devfs_get_ops(void) {
    return &devfs_ops;
}
