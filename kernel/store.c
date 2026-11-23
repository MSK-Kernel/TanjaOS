#include <stdint.h>
#include "../include/store.h"
#include "../include/ata.h"
#include "../include/ahci.h"
#include "../include/multiboot.h"
#include "../include/fs.h"
#include "../include/build_id.h"
#include "../include/iso9660.h"
#include "../include/overlay.h"
#include "../include/vfs.h"

extern void print(const char* s);
extern void print_dec(uint32_t n);
extern void boot_log(const char* msg);
extern void kernel_panic_storage(const char* reason);

// From kernel.c - packs/unpacks the login+hostname config (see there
// for details) so it survives reboots the same way the filesystem does.
extern uint32_t config_store_size(void);
extern uint32_t fs_serialized_size(void);
extern int config_serialize(uint8_t* buf, uint32_t buf_size);
extern int config_deserialize(const uint8_t* buf, uint32_t buf_size);

#define STORE_RESERVED_TAIL_SECTORS 16
#define STORE_FALLBACK_LBA          2048

// Minimum LBA the persistent region is allowed to start at. The installer
// dd's the boot image (GRUB + kernel + tanja.cfg) onto the front of the
// disk with no partition table separating it from anything else, so
// store_lba (computed backward from the end of the disk) must land safely
// past that image or every autosave silently overwrites GRUB's own files.
//
// The boot image is ~7 MiB (tanja-base alone is ~6.8 MiB), so
// 24576 sectors = 12 MiB leaves ~5 MiB of headroom for it to grow (the
// Makefile budget, BOOT_IMAGE_BUDGET_MIB, is 10 MiB).
// This used to be 32768 sectors (16 MiB), which was pure waste: it pushed
// the minimum disk for persistence to just over 64 MiB, so a round 64 MiB
// disk - the size people actually pick - died in a kernel panic even
// though the persistent region itself only needs ~48 MiB. If the image
// ever grows past 10 MiB the Makefile warns at link time; raise this then.
#define STORE_FRONT_RESERVE_SECTORS 24576

static uint32_t store_lba = STORE_FALLBACK_LBA;

// Sized generously above fs_store_size() (checked at runtime below) so
// bumping MAX_F/MAX_D in fs.c doesn't silently overflow this buffer.
/* The Storefile image is streamed to/from disk through a small
 * staging window instead of one whole-image buffer: the old design
 * reserved a 35 MiB .bss buffer for the worst-case image and was the
 * single biggest RAM consumer in the kernel. The on-disk format is
 * unchanged: [16B header][fs image][config image], all streamed in
 * sector-aligned batches. */
#define STORE_STAGE_BYTES (64u * 1024)
#define STORE_CFG_BUF     (16u * 1024)

static uint8_t store_stage[STORE_STAGE_BYTES];
static uint8_t store_cfgbuf[STORE_CFG_BUF];
static int store_enabled = 0;
/* Set once the persistent region has been validated, i.e. the disk area
 * between the boot image and the Storefile may be used for file data. */
static int store_data_ok = 0;
static uint32_t store_sectors = 0;       /* sectors actually used by current image */
static uint32_t store_capacity_sectors = 0; /* maximum reserved region */
static uint32_t store_fs_need = 0;
static uint32_t store_cfg_need = 0;
static uint64_t store_disk_sectors = 0;
static int store_channel = 0;
static int store_drive = 0;

// Which disk backend is actually in use. Legacy ATA (PIO, ports
// 0x1F0/0x170) is tried first since it's simpler and universally
// supported by VM software; AHCI is the fallback for real hardware
// where the BIOS/UEFI only offers AHCI mode with no legacy IDE
// compatibility option at all - increasingly the norm rather than the
// exception on modern machines.
typedef enum { BACKEND_NONE, BACKEND_ATA, BACKEND_AHCI } store_backend_t;
static store_backend_t backend = BACKEND_NONE;

static uint32_t bytes_to_sectors(uint32_t bytes) {
    return (bytes + 511) / 512;
}

/* Smallest disk (in 512-byte sectors) that can hold the reserved
   persistent region at the end plus the boot image guard at the front.
   Both "too small" exits report this computed value instead of a
   hardcoded number: the real requirement moved when the filesystem data
   pool grew, and the old stale figure sent people to disks that then
   panicked anyway. */
static uint64_t store_min_disk_sectors(void) {
    return (uint64_t)store_capacity_sectors
         + STORE_RESERVED_TAIL_SECTORS
         + STORE_FRONT_RESERVE_SECTORS;
}

static uint32_t store_mib_ceil(uint64_t sectors) {
    uint64_t bytes = sectors * 512u;
    return (uint32_t)((bytes + (1024u * 1024u) - 1) / (1024u * 1024u));
}

static uint32_t store_mib_floor(uint64_t sectors) {
    return (uint32_t)((sectors * 512u) / (1024u * 1024u));
}

/* One accurate message for both "too small" exits, then the standard
   storage panic screen. */
static void store_panic_disk_too_small(void) {
    print("TanjaOS needs a disk of at least ");
    print_dec(store_mib_ceil(store_min_disk_sectors()));
    print(" MiB to keep files across reboots (this disk is ");
    print_dec(store_mib_floor(store_disk_sectors));
    print(" MiB)\n");
    kernel_panic_storage(0);
}

static int store_backend_read(uint32_t lba, uint32_t count, void* buf) {
    if (!buf || count == 0) return -1;
    if ((uint64_t)lba + count > store_disk_sectors) return -1;
    uint8_t* p = (uint8_t*)buf;
    while (count) {
        uint32_t chunk = count > 255 ? 255 : count;
        int rc;
        if (backend == BACKEND_ATA)
            rc = ata_read_sectors(store_channel, store_drive, lba, (uint8_t)chunk, p);
        else if (backend == BACKEND_AHCI)
            rc = ahci_read_sectors((uint64_t)lba, (uint16_t)chunk, p);
        else
            return -1;
        if (rc != 0) return rc;
        lba += chunk;
        p += chunk * 512u;
        count -= chunk;
    }
    return 0;
}

static int store_backend_write(uint32_t lba, uint32_t count, const void* buf) {
    if (!buf || count == 0) return -1;
    if ((uint64_t)lba + count > store_disk_sectors) return -1;
    const uint8_t* p = (const uint8_t*)buf;
    while (count) {
        uint32_t chunk = count > 255 ? 255 : count;
        int rc;
        if (backend == BACKEND_ATA)
            rc = ata_write_sectors(store_channel, store_drive, lba, (uint8_t)chunk, p);
        else if (backend == BACKEND_AHCI)
            rc = ahci_write_sectors((uint64_t)lba, (uint16_t)chunk, p);
        else
            return -1;
        if (rc != 0) return rc;
        lba += chunk;
        p += chunk * 512u;
        count -= chunk;
    }
    return 0;
}

#define STORE_HEADER_BYTES 16

static uint32_t le32_at(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void le32_put(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void store_write_header(uint8_t* b, uint32_t payload_bytes) {
    b[0] = 'T'; b[1] = 'J'; b[2] = 'S'; b[3] = '1';
    /* bytes 4..7 = payload size after this 16-byte header */
    le32_put(&b[4], payload_bytes);
    le32_put(&b[8],  TANJA_BUILD_ID_LO);
    le32_put(&b[12], TANJA_BUILD_ID_HI);
}

static int store_header_valid_at(const uint8_t* b) {
    return b[0] == 'T' && b[1] == 'J' && b[2] == 'S' && b[3] == '1';
}

static int store_header_matches_build_at(const uint8_t* b) {
    return le32_at(&b[8])  == (uint32_t)TANJA_BUILD_ID_LO
        && le32_at(&b[12]) == (uint32_t)TANJA_BUILD_ID_HI;
}

/* streaming disk reader
 * Serves sequential bytes of the store image [img_off, img_end) from
 * disk through the 64 KiB staging window (1-128 sector aligned reads). */
typedef struct {
    uint32_t img_off;   /* next byte to deliver                     */
    uint32_t img_end;   /* end of the image (exclusive)             */
    uint32_t win_off;   /* image offset of window start             */
    uint32_t win_len;   /* valid bytes in the window                */
} store_rd_ctx;

static int store_rd_fill(void* vctx, uint8_t* dst, uint32_t len)
{
    store_rd_ctx* c = (store_rd_ctx*)vctx;
    while (len > 0) {
        uint32_t off_in_win;
        uint32_t take;
        if (c->img_off + len > c->img_end) return -1;  /* past image */
        off_in_win = c->img_off - c->win_off;
        if (c->win_len == 0 || off_in_win >= c->win_len) {
            /* refill: aligned batch covering the next request */
            uint32_t sec = c->img_off / 512u;
            uint32_t max_secs = (c->img_end - sec * 512u + 511u) / 512u;
            uint32_t n = max_secs < 128u ? max_secs : 128u;
            if (n < 1u) n = 1u;
            if (store_backend_read(store_lba + sec, n, store_stage) != 0)
                return -1;
            c->win_off = sec * 512u;
            c->win_len = n * 512u;
            off_in_win = c->img_off - c->win_off;
        }
        take = c->win_len - off_in_win;
        if (take > len) take = len;
        {
            uint32_t k;
            for (k = 0; k < take; k++) dst[k] = store_stage[off_in_win + k];
        }
        dst += take; len -= take; c->img_off += take;
    }
    return 0;
}

/* streaming disk writer
 * Stages sequential image bytes in 64 KiB (128-sector) aligned
 * batches written straight to disk. */
typedef struct {
    uint32_t img_off;   /* next image byte to arrive               */
    uint32_t staged;     /* bytes currently in store_stage          */
    uint32_t start_off;  /* image offset of the staged batch        */
    int      failed;      /* any batch write failed                   */
} store_wr_ctx;

static int store_wr_flush(store_wr_ctx* c)
{
    if (c->staged == 0) return 0;
    {
        uint32_t secs = (c->staged + 511u) / 512u;
        uint32_t pad = secs * 512u - c->staged;
        while (pad--) store_stage[c->staged + pad] = 0;
        if (store_backend_write(store_lba + c->start_off / 512u, secs,
                                store_stage) != 0) {
            c->failed = 1;
            return -1;
        }
    }
    c->staged = 0;
    return 0;
}

static void store_wr_sink(void* vctx, const uint8_t* data, uint32_t len)
{
    store_wr_ctx* c = (store_wr_ctx*)vctx;
    while (len > 0) {
        uint32_t take;
        if (c->staged == 0) c->start_off = c->img_off;
        take = STORE_STAGE_BYTES - c->staged;
        if (take > len) take = len;
        {
            uint32_t k;
            for (k = 0; k < take; k++) store_stage[c->staged + k] = data[k];
        }
        c->staged += take; c->img_off += take;
        data += take; len -= take;
        if (c->staged == STORE_STAGE_BYTES)
            store_wr_flush(c);  /* failure recorded in c->failed */
    }
}

/*
 * Boot image write-through.
 *
 * GRUB does not read /boot from the Storefile: it reads the ISO9660 boot image
 * at the front of the disk. Inside TanjaOS, /boot is that image seen through
 * the overlay, and every change lands in the RAM layer and the Storefile only,
 * so GRUB kept the old menu, the old kernel, the old everything. Whenever the
 * saved state is written and something under /boot changed, the changes are
 * also written into the image on disk:
 *   - new directories and files get new directory records; their data goes to
 *     blocks appended to the image (the image's headroom below the
 *     filesystem's data area, see STORE_FRONT_RESERVE_SECTORS);
 *   - edited files are rewritten in place when they still fit, else moved to
 *     fresh blocks at the end of the image;
 *   - deleted files and directories lose their directory record.
 * If the image has no room left, or a directory block is full, nothing is
 * written for that entry and a warning says so.
 */

extern void* malloc(unsigned int n);
extern void free(void* p);

static struct iso_vol boot_vol;
static int boot_vol_state = 0;          /* 0 not tried yet, 1 open, -1 none */
static int bootsync_busy = 0;
static int bootsync_boot = 0;      /* 1 = start-up catch-up: compare large files before nagging */
static int bootsync_force = 0;     /* 1 = also write large files (sync / boot) */
#define BS_DEFER_BYTES (64u * 1024u)

/* failures already reported (path hash + size), so a later unrelated change
 * under /boot does not repeat the same warning */
#define BS_WARN_MAX 8
static uint32_t bs_warn_hash[BS_WARN_MAX], bs_warn_len[BS_WARN_MAX];
static int bs_warn_n = 0;

static uint32_t bs_hash(const char* s) {
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}

/* 1 = first time this (path, len) failure is reported */
static int bs_warn_once(const char* path, uint32_t len) {
    uint32_t h = bs_hash(path);
    int i;
    for (i = 0; i < bs_warn_n; i++)
        if (bs_warn_hash[i] == h && bs_warn_len[i] == len) return 0;
    if (bs_warn_n < BS_WARN_MAX) { bs_warn_hash[bs_warn_n] = h; bs_warn_len[bs_warn_n] = len; bs_warn_n++; }
    return 1;
}

/* The bdev the Storefile (and so the installed boot image) lives on. */
static struct vfs_bdev* bootsync_store_bdev(void) {
    int i, n = vfs_bdev_count();
    for (i = 0; i < n; i++) {
        struct vfs_bdev* d = vfs_bdev_at(i);
        int match = 0;
        if (!d || !d->present || !d->read || !d->write || d->removable) continue;
        if (d->name[0] == 's' && d->name[1] == 'r') continue;
        if (backend == BACKEND_ATA)
            match = ((uintptr_t)d->private == (uintptr_t)((store_channel << 8) | store_drive)) &&
                    d->name[0] == 'a' && d->name[1] == 't' && d->name[2] == 'a';
        else if (backend == BACKEND_AHCI)
            match = d->name[0] == 'a' && d->name[1] == 'h';
        if (match) return d;
    }
    return 0;
}

static int bootsync_open(void) {
    struct vfs_bdev* d;
    if (boot_vol_state != 0) return boot_vol_state > 0;
    boot_vol_state = -1;
    d = bootsync_store_bdev();
    if (d && iso_vol_open(&boot_vol, d)) boot_vol_state = 1;
    return boot_vol_state > 0;
}

/* first block of the image that must NOT be used: where the data area starts */
static uint32_t bootsync_limit_blk(void) {
    return store_front_reserve_sectors() / 4u;          /* 2048-byte blocks */
}

static void bs_say(const char* a, const char* path, const char* b) {
    print("[boot] "); print(a); print(path); print(b); print("\n");
}

static void bs_fail(int rc, uint32_t freeb, uint32_t need, const char* path, const char* what) {
    if (!bs_warn_once(path, (uint32_t)(rc + 100))) return;
    if (rc == -2) {
        print("Storefile: no room in the boot image for "); print(path);
        print(" with "); print_dec(freeb / 1024); print(" KiB free, needs "); print_dec((need + 1023) / 1024);
        print(" KiB, GRUB will not see it\n");
    } else if (rc == -3) {
        print("Storefile: cannot add "); print(path); print(" to the boot image (directory full or name too long); GRUB will not see it\n");
    } else {
        print("Storefile: could not "); print(what); print(" "); print(path); print(" in the boot image\n");
    }
}

static int bs_src(void* ctx, uint32_t off, void* buf, uint32_t n) {
    uint32_t got = 0;
    if (fs_read_file_range((const char*)ctx, off, (char*)buf, n, &got) != 0 || got != n) return -1;
    return 0;
}

/* does the file in the tree match the one in the image? */
static int bs_same(const char* path, const struct iso_node* node, uint32_t len) {
    static uint8_t a[4096], b[4096];
    uint32_t off = 0;
    if (node->size != len) return 0;
    while (off < len) {
        uint32_t n = len - off, ga = 0, gb = 0, i;
        if (n > sizeof a) n = sizeof a;
        if (fs_read_file_range(path, off, (char*)a, n, &ga) != 0 || ga != n) return 0;
        if (iso_v_read_range(&boot_vol, node, off, b, n, &gb) != 0 || gb != n) return 0;
        for (i = 0; i < n; i++) if (a[i] != b[i]) return 0;
        off += n;
    }
    return 1;
}

static void bootsync_file(const char* path) {
    struct iso_node node;
    uint32_t len = fs_get_file_size(path), freeb = 0;
    int rc, exists;

    exists = (iso_v_lookup(&boot_vol, path, &node) == 0);
    if (exists && (node.is_dir || node.is_link)) return;

    /* A big file arrives in many chunks, and writing it to the image after
       each one would take forever and leave a half-copied file where GRUB
       could see it (think: cp newkernel /boot/kernel/kernel). So anything
       involving a large file - the new one or the one it replaces - waits
       for an explicit `sync`. */
    if ((len > BS_DEFER_BYTES || (exists && node.size > BS_DEFER_BYTES)) && !bootsync_force) {
        if (bootsync_boot && exists && bs_same(path, &node, len)) return;   /* already in the image */
        if (bs_warn_once(path, 1)) {
            print("[boot] "); print(path);
            print(" is large: run `sync` to write it to the boot image\n");
        }
        return;
    }

    if (exists) {
        if (bs_same(path, &node, len)) return;
        /* never leave GRUB an empty menu */
        if (len == 0 && path[6] == 'g' && path[7] == 'r' && path[8] == 'u' && path[9] == 'b' &&
            path[10] == '/' && path[11] == 'g' && path[12] == 'r' && path[13] == 'u' && path[14] == 'b' &&
            path[15] == '.' && path[16] == 'c' && path[17] == 'f' && path[18] == 'g' && path[19] == 0)
            return;
    }
    rc = iso_v_put_file(&boot_vol, path, len, bs_src, (void*)path, bootsync_limit_blk(), &freeb);
    if (rc == 0) bs_say("", path, exists ? " written to the boot image" : " added to the boot image");
    else bs_fail(rc, freeb, len, path, "write");
}

static void bootsync_walk(const char* dir, int depth) {
    char* list;
    uint32_t sz = 0, i = 0;
    if (depth > 6) return;
    list = (char*)malloc(16400);
    if (!list) return;
    if (ovl_upper_list(dir, list, &sz) != 0) { free(list); return; }
    if (sz > 16399) sz = 16399;
    list[sz] = 0;
    while (i < sz) {
        char path[160];
        uint32_t s = i, e, nl, k, m = 0;
        int is_dir;
        while (i < sz && list[i] != '\n') i++;
        e = i;
        if (i < sz) i++;
        if (e == s) continue;
        is_dir = (list[e - 1] == '/');
        nl = e - s - (is_dir ? 1 : 0);
        for (k = 0; dir[k] && m < sizeof path - 2; k++) path[m++] = dir[k];
        if (!(m == 1 && path[0] == '/')) path[m++] = '/';
        if (m + nl >= sizeof path) continue;
        for (k = 0; k < nl; k++) path[m++] = list[s + k];
        path[m] = 0;
        if (is_dir) {
            struct iso_node nd;
            if (iso_v_lookup(&boot_vol, path, &nd) != 0) {
                uint32_t freeb = 0;
                int rc = iso_v_make_dir(&boot_vol, path, bootsync_limit_blk(), &freeb);
                if (rc == 0) bs_say("", path, " added to the boot image");
                else { bs_fail(rc, freeb, 2048, path, "create"); continue; }
            } else if (!nd.is_dir) continue;
            bootsync_walk(path, depth + 1);
        } else {
            bootsync_file(path);
        }
    }
    free(list);
}

/* rm in the overlay only hides an ISO entry; GRUB reads the image itself, so
 * hidden /boot entries are also taken out of the image's directory records.
 * /boot and /boot/grub themselves are never removed (that would leave nothing
 * to boot). */
static int bootsync_protect_warned = 0;

static void bootsync_prune(void) {
    char path[160];
    int i, n = ovl_whiteout_count();
    for (i = 0; i < n; i++) {
        const char* w = ovl_whiteout_at(i);
        struct iso_node node;
        unsigned k;
        if (!w) continue;
        for (k = 0; w[k] && k < sizeof path - 1; k++) path[k] = w[k];
        path[k] = 0;
        if (w[k]) continue;                                 /* too long: skip */
        if (path[0] != '/' || path[1] != 'b' || path[2] != 'o' || path[3] != 'o' ||
            path[4] != 't' || path[5] != '/' || path[6] == 0) continue;   /* only /boot/... */
        if (fs_file_exists(path) || fs_directory_exists(path)) continue;  /* made again since */
        if (iso_v_lookup(&boot_vol, path, &node) != 0) continue;           /* already gone */
        if (path[6] == 'g' && path[7] == 'r' && path[8] == 'u' && path[9] == 'b' && path[10] == 0) {
            if (!bootsync_protect_warned) {
                bootsync_protect_warned = 1;
                print("[boot] /boot/grub is not removed from the boot image (GRUB needs it)\n");
            }
            continue;
        }
        if (!node.is_dir && node.size > BS_DEFER_BYTES && !bootsync_force) {
            if (bs_warn_once(path, 2)) {
                print("[boot] "); print(path);
                print(" is large: run `sync` to remove it from the boot image\n");
            }
            continue;
        }
        if (iso_v_remove_entry(&boot_vol, path) == 0) bs_say("", path, " removed from the boot image");
        else bs_say("could not remove ", path, " from the boot image");
    }
}

static void store_sync_boot_files(void) {
    struct vfs_bdev* d;
    const char* a;
    const char* b;

    if (bootsync_busy) return;
    /* Only when /boot is this disk's own boot image seen through the overlay.
       Booted from some other ISO (installer / live medium in a drive) it is
       that medium's copy, and must never be copied across. */
    if (!ovl_active_now()) return;
    d = bootsync_store_bdev();
    if (!d) return;
    a = iso_device_name();
    b = d->name;
    while (*a && *a == *b) { a++; b++; }
    if (*a || *b) return;

    bootsync_busy = 1;
    if (bootsync_open()) {
        bootsync_walk("/boot", 0);
        bootsync_prune();
    }
    bootsync_busy = 0;
}

/* Mirror /boot into the boot image on disk (no-op unless persistence is on). */
void store_sync_boot(void) {
    if (store_enabled) store_sync_boot_files();
}

/* same, but also writes large files (explicit `sync`, boot-time catch-up) */
void store_sync_boot_force(void) {
    if (!store_enabled) return;
    bootsync_force = 1;
    store_sync_boot_files();
    bootsync_force = 0;
}

/* start-up: bring the image up to date with what an earlier session left in
 * the saved state (large files still wait for an explicit `sync`) */
static void store_sync_boot_startup(void) {
    if (!store_enabled) return;
    bootsync_boot = 1;
    store_sync_boot_files();
    bootsync_boot = 0;
}

void store_autosave(void) {
    if (!store_enabled) return;

    /* Serialize only the live filesystem contents. This is the main
 * performance fix: mkdir/rm used to rewrite the entire 31 MB maximum
 * filesystem image every time. v4 writes only metadata + active file
 * bytes, then only the sectors containing that compact image.
 */
    uint32_t fs_bytes = fs_serialized_size();
    uint32_t payload_bytes = fs_bytes + store_cfg_need;
    uint32_t total_bytes = STORE_HEADER_BYTES + payload_bytes;
    uint32_t sectors = bytes_to_sectors(total_bytes);
    store_wr_ctx wr;
    uint8_t hdr[STORE_HEADER_BYTES];

    if (fs_bytes > store_fs_need ||
        payload_bytes > (store_capacity_sectors * 512u) - STORE_HEADER_BYTES ||
        sectors > store_capacity_sectors) {
        return;
    }

    store_write_header(hdr, payload_bytes);

    wr.img_off = 0; wr.staged = 0; wr.start_off = 0;
    wr.failed = 0;
    store_wr_sink(&wr, hdr, STORE_HEADER_BYTES);
    fs_serialize_sink(&wr, store_wr_sink);
    if (config_serialize(store_cfgbuf, sizeof store_cfgbuf) != 0)
        return;
    store_wr_sink(&wr, store_cfgbuf, store_cfg_need);
    if (store_wr_flush(&wr) != 0 || wr.failed)
        return;

    store_sectors = sectors;

    /* A failed persistence write must never make the kernel continue
       as if the on-disk image were valid. */
    store_enabled = 0;   /* set back to 1 only after the write lands */
    {
        /* verify the header sector by reading it back */
        uint8_t chk[512];
        if (store_backend_read(store_lba, 1, chk) == 0 &&
            store_header_valid_at(chk) &&
            le32_at(&chk[4]) == payload_bytes)
            store_enabled = 1;
    }
}

/* `sync`: save the state, then make sure the boot image on disk matches
 * /boot (also covers writes that do not go through the normal file ops). */
void store_save(void) {
    store_autosave();
    store_sync_boot_force();
}

int store_is_persistent(void) {
    return store_enabled;
}

// Print which of the 4 legacy IDE slots (primary/secondary,
// master/slave) got used, so it's visible on the boot log instead of
// being a silent guess. Handy for diagnosing "why is this running from RAM?"
// across different VM software that orders IDE devices differently.
static void log_slot(int channel, int drive) {
    boot_log(channel == 0
        ? (drive == 0 ? "Storefile: using primary master"
                      : "Storefile: using primary slave")
        : (drive == 0 ? "Storefile: using secondary master"
                      : "Storefile: using secondary slave"));
}

static int has_word(const char* s, const char* w) {
    int n;
    if (!s || !w) return 0;
    n = 0; while (w[n]) n++;
    while (*s) {
        int i = 0;
        while (i < n && (s[i] | 32) == (w[i] | 32)) i++;
        if (i == n) return 1;
        s++;
    }
    return 0;
}

/* Whole-word match ("reset" must not match "preset"). */
static int has_exact_word(const char* s, const char* w) {
    int n = 0;
    if (!s || !w) return 0;
    while (w[n]) n++;
    while (*s) {
        int i = 0;
        while (i < n && (s[i] | 32) == (w[i] | 32)) i++;
        if (i == n && (s[n] == 0 || s[n] == ' ' || s[n] == '\t')) return 1;
        while (*s && *s != ' ' && *s != '\t') s++;      /* skip to the next word */
        while (*s == ' ' || *s == '\t') s++;
    }
    return 0;
}

/* Is `word` on the kernel command line or in a module string? */
static int boot_has_flag(uint32_t mb_magic, uint32_t mb_addr, const char* word) {
    multiboot_info_t* mbi;
    if (mb_magic != MULTIBOOT_BOOTLOADER_MAGIC || mb_addr == 0) return 0;
    mbi = (multiboot_info_t*)(uintptr_t)mb_addr;
    if (mbi->cmdline && has_exact_word((const char*)(uintptr_t)mbi->cmdline, word)) return 1;
    if ((mbi->flags & MULTIBOOT_FLAG_MODS) && mbi->mods_count > 0) {
        multiboot_module_t* mods = (multiboot_module_t*)(uintptr_t)mbi->mods_addr;
        int i;
        for (i = 0; i < (int)mbi->mods_count; i++) {
            const char* m = (const char*)(uintptr_t)mods[i].string;
            if (m && has_exact_word(m, word)) return 1;
        }
    }
    return 0;
}

static int boot_is_livecd(uint32_t mb_magic, uint32_t mb_addr) {
#ifdef TANJA_LIVECD
    /* A LiveCD image must never touch storage: the stick it was written to
 * is the boot medium, and a machine running it may have any disk. This
 * is compiled in, so no boot configuration can turn it back on. */
    (void)mb_magic; (void)mb_addr;
    return 1;
#endif
    /* Explicit live/livecd boot requests remain RAM-only. Otherwise,
       persistence is automatic when store_init has found a usable disk.
       Do not classify an optical boot device as LiveCD here: a VM can
       boot its installer ISO while also having a writable disk.img attached. */
    if (mb_magic != MULTIBOOT_BOOTLOADER_MAGIC || mb_addr == 0)
        return 1; /* without Multiboot metadata, avoid guessing */

    multiboot_info_t* mbi = (multiboot_info_t*)(uintptr_t)mb_addr;

    if (mbi->cmdline && (has_word((const char*)(uintptr_t)mbi->cmdline, "live") ||
                         has_word((const char*)(uintptr_t)mbi->cmdline, "livecd")))
        return 1;

    if (mbi->cmdline && (has_word((const char*)(uintptr_t)mbi->cmdline, "install") ||
                         has_word((const char*)(uintptr_t)mbi->cmdline, "persist")))
        return 0;

    if ((mbi->flags & MULTIBOOT_FLAG_MODS) && mbi->mods_count > 0) {
        multiboot_module_t* mods =
            (multiboot_module_t*)(uintptr_t)mbi->mods_addr;
        int i;
        for (i = 0; i < (int)mbi->mods_count; i++) {
            const char *s = (const char*)(uintptr_t)mods[i].string;
            if (!s) continue;
            if (has_word(s, "live")) return 1;
            if (has_word(s, "storefile") || has_word(s, "install") ||
                has_word(s, "persist"))
                return 0;
        }
    }

    /* No explicit live marker: if a disk is found, use persistence. */
    return 0;
}

void store_init(uint32_t mb_magic, uint32_t mb_addr) {
    store_fs_need = fs_store_size();       /* maximum possible v4 image */
    store_cfg_need = config_store_size();

    uint32_t capacity_bytes = STORE_HEADER_BYTES + store_fs_need + store_cfg_need;
    store_capacity_sectors = bytes_to_sectors(capacity_bytes);
    store_sectors = 1; /* first read is just the header */

    ata_init();

    ata_drive_info_t info;
    int found = 0;
    uint64_t disk_sectors = 0;
    int ch, dr;
    for (ch = 0; ch < 2 && !found; ch++) {
        for (dr = 0; dr < 2 && !found; dr++) {
            if (ata_detect_drive(ch, dr, &info) == 0 && info.present) {
                store_channel = ch;
                store_drive = dr;
                backend = BACKEND_ATA;
                disk_sectors = info.sectors;
                found = 1;
            }
        }
    }

    /* USB Live boot: the stick the image was written to appears as a
       removable ATA device (or its model string identifies it as USB /
       flash). Never use it for persistent storage; run from RAM instead.
       Fixed hard disks still get persistence. */
    if (found && backend == BACKEND_ATA &&
        (info.removable ||
         has_word(info.model, "usb") ||
         has_word(info.model, "flash") ||
         has_word(info.model, "mass storage") ||
         has_word(info.model, "udisk"))) {
        store_enabled = 0;
        fs_init();
        fs_seed_home();
        boot_log("Storefile: booted from USB/removable media, running from RAM (nothing is written to the stick)");
        return;
    }

    if (!found && ahci_init() == 0) {
        backend = BACKEND_AHCI;
        disk_sectors = ahci_get_sector_count();
        found = 1;
        boot_log("Storefile: using AHCI drive");
    }

    if (!found) {
        /* No writable ATA/AHCI disk exists. Keep the system usable as a
           RAM session; persistence is enabled automatically only after a
           disk is detected and its reserved region is validated. */
        store_enabled = 0;
        fs_init();
        fs_seed_home();
        boot_log("Storefile: no usable disk found, running from RAM");
        return;
    }

    store_disk_sectors = disk_sectors;
    if (backend == BACKEND_ATA) log_slot(store_channel, store_drive);

    /* LiveCD / live USB: the stick the ISO was written to is a real disk,
       but it is the boot medium, not an install target. Never persist. */
    if (boot_is_livecd(mb_magic, mb_addr)) {
        store_enabled = 0;
        fs_init();
        fs_seed_home();
#ifdef TANJA_LIVECD
        boot_log("Storefile: LiveCD build - running from RAM, no disk is ever written");
#else
        boot_log("Storefile: LiveCD session, running from RAM (nothing is written to disk)");
#endif
        return;
    }

    /* Reserve enough room for the worst-case filesystem, but do NOT read
 * that whole region at boot. The first sector contains the exact
 * compact image length; only that many sectors are read below.
 */
    if (disk_sectors > (uint64_t)(store_capacity_sectors +
                                  STORE_RESERVED_TAIL_SECTORS + 32)) {
        store_lba = (uint32_t)(disk_sectors - store_capacity_sectors -
                               STORE_RESERVED_TAIL_SECTORS);
    } else {
        if (boot_is_livecd(mb_magic, mb_addr)) {
            store_enabled = 0;
            fs_init();
            fs_seed_home();
            boot_log("Storefile: This session will run from RAM");
            return;
        }
        store_panic_disk_too_small();
    }

    store_data_ok = 1;

    // store_lba is anchored to the END of the disk, but the installer put
    // the boot image (GRUB/kernel/tanja.cfg) at the START with nothing
    // reserving space in between. On a disk that isn't comfortably bigger
    // than store_capacity_sectors, store_lba lands inside that boot image
    // and every autosave overwrites it -- GRUB boots fine on the current
    // session (it already finished reading before this runs) but the
    // *next* boot drops to a bare grub> prompt. Refuse to run persistent
    // on a disk that small instead of corrupting it silently.
    if (store_lba < STORE_FRONT_RESERVE_SECTORS) {
        if (boot_is_livecd(mb_magic, mb_addr)) {
            store_enabled = 0;
            fs_init();
            fs_seed_home();
            boot_log("Storefile: disk too small for safe persistence, running from RAM");
            return;
        }
        store_panic_disk_too_small();
    }

    /* ---- Load the saved state - and never destroy one we cannot read ----
     *
     * Only a disk with NO Storefile (blank, or never used by TanjaOS) may be
     * started fresh and written. If a valid Storefile is there but cannot be
     * loaded (written by another build, bigger than this machine's file
     * memory, damaged header, unreadable sector), starting fresh and saving
     * would silently overwrite the user's data, so instead the system runs
     * from RAM, leaves the disk untouched and says why. The old behaviour
     * (discard and start over) is still available on purpose: put the word
     * "reset" on the kernel command line.
     */
    const char* keep_reason = 0;
    if (store_backend_read(store_lba, 1, store_stage) != 0) {
        keep_reason = "the disk could not be read";
    } else if (!store_header_valid_at(store_stage)) {
        boot_log("Storefile: on-disk image starting from a fresh condition");
    } else if (!store_header_matches_build_at(store_stage)) {
        keep_reason = "it was written by a different TanjaOS build";
    } else {
        /* Header validated: the payload streams straight from disk through
           the staging window - no whole-image RAM buffer. */
        uint32_t payload_bytes = le32_at(&store_stage[4]);
        if (payload_bytes < 8 + store_cfg_need ||
            payload_bytes > store_fs_need + store_cfg_need ||
            bytes_to_sectors(STORE_HEADER_BYTES + payload_bytes) > store_capacity_sectors) {
            keep_reason = "its header is inconsistent";
        } else {
            uint32_t total_bytes = STORE_HEADER_BYTES + payload_bytes;
            store_rd_ctx rd;
            store_sectors = bytes_to_sectors(total_bytes);
            rd.img_off  = STORE_HEADER_BYTES;
            rd.img_end  = total_bytes;
            rd.win_off  = 0;
            rd.win_len  = 0;

            /* Filesystem tables must exist before deserialize fills them.
             *
             * While loading, the disk is READ-ONLY: if the saved files do not
             * fit this machine's RAM pool, the file layer's fallback is to
             * spill them onto the data blocks - but the allocator state that
             * says which of those blocks the saved image already uses is
             * restored only at the very end of the image, so spilling now
             * would overwrite the saved state's own disk-backed files. A
             * load that cannot be done in RAM must fail, not write. */
            store_data_ok = 0;
            fs_init();
            if (fs_deserialize_fill(&rd, store_rd_fill) == 0 &&
                store_rd_fill(&rd, store_cfgbuf, store_cfg_need) == 0 &&
                config_deserialize(store_cfgbuf, store_cfg_need) == 0) {
                store_data_ok = 1;
                store_enabled = 1;
                boot_log("Storefile: loaded saved state from disk");
                store_sync_boot_startup();   /* edits saved by an earlier session that never reached the image */
                return;
            }
            keep_reason = "it does not fit in this machine's file memory, or is damaged";
        }
    }

    if (keep_reason) {
        if (boot_has_flag(mb_magic, mb_addr, "reset")) {
            boot_log("Storefile: 'reset' given - discarding the saved state on disk");
            store_data_ok = 1;
        } else {
            store_enabled = 0;       /* no autosave ... */
            store_data_ok = 0;       /* ... and no big-file spill onto the disk */
            fs_init();               /* (flags first: seeding home/ on a small */
            fs_seed_home();          /*  pool would otherwise spill to disk)  */
            {
                char why[160];
                const char* pre = "Storefile: WARNING - the saved data on this disk cannot be loaded: ";
                int n = 0, k;
                for (k = 0; pre[k] && n < 120; k++) why[n++] = pre[k];
                for (k = 0; keep_reason[k] && n < 158; k++) why[n++] = keep_reason[k];
                why[n] = 0;
                boot_log(why);
            }
            boot_log("Storefile: the disk was NOT modified. Running from RAM only - nothing will be saved.");
            boot_log("Storefile: add the word 'reset' to the kernel line to discard it and start fresh.");
            return;
        }
    }

    /* No usable disk state. Start from factory home/ contents, then
       optionally accept a compact v4 Storefile boot module. */
    fs_init();
    fs_seed_home();

    if (mb_magic == MULTIBOOT_BOOTLOADER_MAGIC && mb_addr) {
        multiboot_info_t* mbi = (multiboot_info_t*)(uintptr_t)mb_addr;
        if ((mbi->flags & MULTIBOOT_FLAG_MODS) && mbi->mods_count > 0) {
            multiboot_module_t* mods =
                (multiboot_module_t*)(uintptr_t)mbi->mods_addr;
            uint32_t mod_start = mods[0].mod_start;
            uint32_t mod_end = mods[0].mod_end;
            uint32_t mod_size = mod_end - mod_start;

            if (mod_size >= 8) {
                if (fs_deserialize((const uint8_t*)(uintptr_t)mod_start,
                                   mod_size) == 0)
                    boot_log("Storefile: seeded state from boot module");
                else
                    boot_log("Storefile: boot module wasn't a valid image, starting empty");
            }
        }
    }

    store_enabled = 1;
    store_autosave();
    boot_log("Storefile: persistence enabled, initial state written to disk");
    store_sync_boot_startup();
}

/*
 * Raw block access for file data that does not fit the RAM pool.
 *
 * The filesystem keeps small files in RAM and spills large ones (package
 * payloads, Doom, tarballs) onto the disk itself. That area is everything
 * between the boot image at the front - the installer dd's GRUB plus the
 * kernel there - and the reserved Storefile region at the end, so its
 * size is whatever the disk has: a 1 TB disk means 1 TB of files even
 * with 64 MiB of RAM.
 */
#define STORE_DATA_GUARD_SECTORS 64

int store_data_ready(void) {
    return store_data_ok && backend != BACKEND_NONE;
}

uint32_t store_data_first_sector(void) {
    return STORE_FRONT_RESERVE_SECTORS;
}

uint32_t store_data_sector_count(void) {
    if (!store_data_ready()) return 0;
    if (store_lba <= STORE_FRONT_RESERVE_SECTORS + STORE_DATA_GUARD_SECTORS)
        return 0;
    return store_lba - STORE_DATA_GUARD_SECTORS - STORE_FRONT_RESERVE_SECTORS;
}

int store_data_read(uint32_t lba, uint32_t count, void* buf) {
    if (!store_data_ready() || !buf || !count) return -1;
    if (lba < STORE_FRONT_RESERVE_SECTORS) return -1;
    if ((uint64_t)lba + count > (uint64_t)store_lba) return -1;
    return store_backend_read(lba, count, buf);
}

int store_data_write(uint32_t lba, uint32_t count, const void* buf) {
    if (!store_data_ready() || !buf || !count) return -1;
    if (lba < STORE_FRONT_RESERVE_SECTORS) return -1;
    if ((uint64_t)lba + count > (uint64_t)store_lba) return -1;
    return store_backend_write(lba, count, buf);
}



/* ---- reading a Storefile from another disk (fs/tanjafs.c) ---- */

/* Where this build would put the Storefile on a disk of `disk_sectors`
 * sectors (same arithmetic as store_init). 0 = disk too small. */
uint32_t store_locate_lba(uint64_t disk_sectors) {
    uint32_t cap = bytes_to_sectors(STORE_HEADER_BYTES + fs_store_size() + config_store_size());
    if (disk_sectors <= (uint64_t)cap + STORE_RESERVED_TAIL_SECTORS + 32) return 0;
    return (uint32_t)(disk_sectors - cap - STORE_RESERVED_TAIL_SECTORS);
}

uint32_t store_front_reserve_sectors(void) { return STORE_FRONT_RESERVE_SECTORS; }

/* first sector of a Storefile: 2 = valid and written by this very build,
 * 1 = valid but another build, 0 = not a Storefile. *payload gets the
 * image length that follows the 16-byte header. */
int store_check_header(const uint8_t* sector, uint32_t* payload) {
    if (!store_header_valid_at(sector)) return 0;
    if (payload) *payload = le32_at(&sector[4]);
    return store_header_matches_build_at(sector) ? 2 : 1;
}
