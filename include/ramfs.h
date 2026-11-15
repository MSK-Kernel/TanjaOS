#ifndef RAMFS_H
#define RAMFS_H

#include "vfs.h"

/* The original in-memory filesystem, now a pure VFS backend.
 * Call ramfs_init() once at boot, then mount it via vfs_mount_root(). */
void ramfs_init(void);
const struct vfs_ops* ramfs_get_ops(void);
void* ramfs_get_sb(void);

/* Pool setup (must be called before ramfs_init) */
void ramfs_set_pool(void* base, uint32_t bytes);
uint32_t ramfs_pool_bytes(void);
uint32_t ramfs_pool_max_bytes(void);

/* Seeding and persistence helpers still needed by the kernel */
void ramfs_seed_home(void);
extern uint32_t ramfs_seed_skipped;

/* Serialization (Storefile) */
uint32_t ramfs_store_size(void);
int ramfs_serialize(uint8_t* buf, uint32_t buf_size);
int ramfs_deserialize(const uint8_t* buf, uint32_t buf_size);
int ramfs_serialize_sink(void* ctx, void (*emit)(void*, const uint8_t*, uint32_t));
int ramfs_deserialize_fill(void* ctx, int (*fill)(void*, uint8_t*, uint32_t));

/* Shared state the rest of the kernel still expects */
extern int fs_cur_uid;
extern int fs_errno;

#endif /* RAMFS_H */
