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

#endif
