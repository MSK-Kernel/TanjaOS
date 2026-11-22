#ifndef ISO9660_H
#define ISO9660_H

#include <stdint.h>
#include "vfs.h"

/* TanjaOS ISO9660 reader
 *
 * Read-only access to the ISO the machine was booted from (a CD/DVD on a
 * legacy IDE channel, or a hybrid ISO that was written to a disk).
 * Names come from Rock Ridge when the image has it (what grub-mkrescue /
 * xorriso write), else from Joliet, else from the plain ISO9660 names with
 * the ";1" version suffix stripped.
 *
 * Paths are absolute and start at the root of the ISO: "/", "/boot",
 * "/boot/grub/grub.cfg". The overlay layer (fs/overlay.c) puts this under
 * the TanjaOS root.
 */

struct iso_node {
    uint32_t extent;     /* first 2048-byte block of the data */
    uint32_t size;       /* bytes (directories: bytes of directory data) */
    uint16_t mode;       /* permission bits, 0777 mask */
    uint8_t  is_dir;
    uint8_t  is_link;    /* Rock Ridge symlink: shown as an empty file */
};

/* State of one ISO volume. The boot volume lives inside iso9660.c; a second
 * one (a disk or CD that gets mounted later) is a struct iso_vol owned by
 * the caller and opened with iso_vol_open(). */
struct iso_vol {
    struct vfs_bdev* dev;
    int      active;
    uint32_t root_extent, root_size;
    int      style;          /* 0 plain, 1 joliet, 2 rockridge */
    uint32_t rr_skip;
    char     label[40];
};

/* Look for an ISO9660 volume on every registered block device (optical
 * drives first). Returns 1 if one was found and is now the active volume,
 * 0 otherwise. Safe to call again after the device table was rebuilt. */
int iso_probe(void);

int iso_available(void);
const char* iso_device_name(void);
const char* iso_volume_label(void);     /* "" if none */
const char* iso_name_style(void);       /* "rockridge", "joliet" or "plain" */

/* 0 = found, -1 = no such path. */
int iso_lookup(const char* path, struct iso_node* out);

/* Read up to cap bytes at offset. *got receives the byte count (0 at EOF).
 * Returns 0 or -1 on an I/O error. */
int iso_read_range(const struct iso_node* n, uint32_t offset,
                   void* buf, uint32_t cap, uint32_t* got);

/* Directory listing in the same format as ramfs: one name per line,
 * directories first and with a trailing '/'. At most cap-1 bytes are
 * written; *size receives the length. 0 = ok, -1 = not a directory. */
int iso_list(const char* path, char* buf, uint32_t cap, uint32_t* size);

/* 1 if the directory has no entries besides . and .. */
int iso_dir_is_empty(const char* path);

/* The same operations on an explicitly opened volume. iso_vol_open()
 * returns 1 if dev holds an ISO9660 image (also a hybrid one at the start
 * of a disk) and fills *v. */
int iso_vol_open(struct iso_vol* v, struct vfs_bdev* dev);
int iso_v_lookup(struct iso_vol* v, const char* path, struct iso_node* out);
int iso_v_list(struct iso_vol* v, const char* path, char* buf, uint32_t cap, uint32_t* size);
int iso_v_dir_is_empty(struct iso_vol* v, const char* path);
int iso_v_read_range(struct iso_vol* v, const struct iso_node* n, uint32_t offset,
                     void* buf, uint32_t cap, uint32_t* got);
const char* iso_v_name_style(const struct iso_vol* v);

/* Overwrite an existing plain file of a volume that sits on a writable disk
 * (the installed boot image), in place. The new contents must fit in the
 * blocks the file already occupies: 0 = ok, -1 = error / not a plain file,
 * -2 = too big (*cap_out = bytes the file can hold). */
/* Remove a file or directory from a volume on a writable disk by deleting its
 * directory record (the data blocks are left unreferenced). 0 = ok. */
int iso_v_remove_entry(struct iso_vol* v, const char* path);
int iso_v_update_file(struct iso_vol* v, const char* path,
                      const void* data, uint32_t len, uint32_t* cap_out);

#endif
