#ifndef OVERLAY_H
#define OVERLAY_H

#include <stdint.h>

/* Overlay root: files of the ISO the system was booted from appear in the
 * root directory (read-only underneath, copy-on-write into RAM). See
 * fs/overlay.c. */

/* Probe for an ISO and, if found, put the overlay under the root mount.
 * Returns 1 if the overlay is active. */
int ovl_init(void);
int ovl_active_now(void);

/* For writes that do not go through the VFS ops (fs_write_at,
 * fs_prealloc_file): copy an ISO file up / create parents. Absolute path. */
int ovl_prepare_write(const char* abs);

/* Direct data pointer for a file that only exists on the ISO. */
int ovl_is_iso_only(const char* abs);
const char* ovl_iso_data_ptr(const char* abs, uint32_t* size_out);

/* Drop the RAM copy of a file so the ISO original shows through again
 * (no whiteout). Absolute path. */
int ovl_revert(const char* abs);

/* Entries of a directory that live in the RAM layer only (copied-up or new),
 * listed like a directory listing. */
int ovl_upper_list(const char* p, char* out, uint32_t* size);

/* ISO paths hidden by deletes (whiteouts). */
int ovl_whiteout_count(void);
const char* ovl_whiteout_at(int i);

#endif
