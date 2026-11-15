#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include <stddef.h>

/* TanjaOS Virtual File System
 *
 * A mount-based VFS with block-device support. Inspired by the ideas
 * in FreeBSD's VFS (vnodes, mount points, filesystem ops) but sized
 * for a small freestanding kernel.
 *
 * - Multiple filesystems can be mounted at arbitrary paths.
 * - Block devices (ATA / AHCI drives) are registered and can be
 *   mounted with a filesystem type.
 * - The public fs_* API is implemented entirely on top of this layer.
 */

#define VFS_MAX_MOUNTS   16
#define VFS_MAX_BDEVS    8
#define VFS_PATH_MAX     256
#define VFS_NAME_MAX     64

typedef enum {
    VFS_OK = 0,
    VFS_ERR = -1,
    VFS_ENOENT = -2,
    VFS_EEXIST = -3,
    VFS_EACCES = -4,
    VFS_ENOSPC = -5,
    VFS_EINVAL = -6,
    VFS_EBUSY  = -7,
    VFS_ENOTDIR = -8
} vfs_err_t;

/* ---------- Block device ---------- */

struct vfs_bdev {
    char name[32];              /* e.g. "ata0", "ahci0" */
    char model[48];
    uint64_t sectors;           /* 512-byte sectors */
    int present;
    int (*read)(struct vfs_bdev* dev, uint64_t lba, uint32_t count, void* buf);
    int (*write)(struct vfs_bdev* dev, uint64_t lba, uint32_t count, const void* buf);
    void* private;
};

/* ---------- Filesystem operations (vnode-style) ---------- */

struct vfs_ops {
    const char* name;           /* "ramfs", "tmpfs", "devfs", ... */

    int (*create_file)(void* sb, const char* path);
    int (*create_dir)(void* sb, const char* path);
    int (*delete_file)(void* sb, const char* path);
    int (*delete_dir)(void* sb, const char* path);
    int (*delete_dir_recursive)(void* sb, const char* path);
    int (*write_file)(void* sb, const char* path, const char* data, uint32_t size);
    int (*read_file)(void* sb, const char* path, char* buffer, uint32_t* size);
    int (*read_file_range)(void* sb, const char* path, uint32_t offset,
                           char* buffer, uint32_t capacity, uint32_t* size);
    int (*append_file)(void* sb, const char* path, const char* data, uint32_t len);
    uint32_t (*get_file_size)(void* sb, const char* path);
    int (*file_exists)(void* sb, const char* path);
    int (*dir_exists)(void* sb, const char* path);
    int (*dir_is_empty)(void* sb, const char* path);
    int (*list_dir)(void* sb, const char* path, char* buffer, uint32_t* size);
    int (*chmod)(void* sb, const char* path, int mode);
    int (*chown)(void* sb, const char* path, int uid);
    int (*get_owner)(void* sb, const char* path);
    int (*get_mode)(void* sb, const char* path);
    int (*write_at)(void* sb, const char* path, uint32_t offset, const char* data, uint32_t len);
    int (*prealloc)(void* sb, const char* path, uint32_t size);

    /* optional: called when the filesystem is mounted / unmounted */
    int (*mount)(void* sb, struct vfs_bdev* dev, const char* opts);
    void (*unmount)(void* sb);
};

/* ---------- Mount ---------- */

struct vfs_mount {
    char mountpoint[VFS_PATH_MAX];
    const struct vfs_ops* ops;
    void* sb;
    struct vfs_bdev* dev;       /* NULL if not device-backed */
    char fstype[32];
    int used;
    int flags;
};

/* ---------- VFS core ---------- */

void vfs_init(void);

/* Register the root filesystem. */
int vfs_mount_root(const struct vfs_ops* ops, void* sb);

/* Mount a filesystem at an existing directory.
   dev may be NULL for in-memory filesystems. */
int vfs_mount(const char* mountpoint, const char* fstype,
              struct vfs_bdev* dev, const char* opts);

/* Unmount. */
int vfs_unmount(const char* mountpoint);

/* Resolve an absolute path to a mount + relative path inside it. */
struct vfs_mount* vfs_resolve(const char* abs_path, char* rel_buf);

/* List currently mounted filesystems into a caller buffer
   (newline-separated "mountpoint fstype device" lines). */
int vfs_list_mounts(char* buf, uint32_t* size);

/* ---------- Block devices ---------- */

int vfs_register_bdev(struct vfs_bdev* dev);
struct vfs_bdev* vfs_find_bdev(const char* name);
int vfs_list_bdevs(char* buf, uint32_t* size);   /* for lsblk */
int vfs_bdev_count(void);                        /* for devfs */
struct vfs_bdev* vfs_bdev_at(int index);         /* NULL if out of range */

/* devfs: the filesystem mounted at /dev. One node per registered block
   device (fs_init() registers the type and mounts it). */
const struct vfs_ops* devfs_get_ops(void);

/* Probe ATA / AHCI and register any drives that are present.
   Called once at boot. */
void vfs_probe_drives(void);

/* Look up a registered filesystem type by name ("ramfs", "tmpfs", ...). */
const struct vfs_ops* vfs_find_fstype(const char* name);
int vfs_register_fstype(const struct vfs_ops* ops);

#endif /* VFS_H */
