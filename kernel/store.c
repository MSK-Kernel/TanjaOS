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
 * GRUB does not read /boot/grub/grub.cfg from the Storefile: it reads it from
 * the ISO9660 boot image at the front of the disk. An edit made inside
 * TanjaOS only lands in the RAM filesystem (and from there in the Storefile),
 * so GRUB kept showing the original menu after a reboot. Whenever the saved
 * state is written, the boot config files that were edited are therefore also
 * written into the boot image, in place.
 *
 * An ISO cannot grow a file, so the new contents must fit in the blocks the
 * file already owns (2048 bytes for a config file smaller than that). If it
 * does not fit, nothing is written to the boot image and a warning says so.
 */
#define BOOTSYNC_BUF (8u * 2048u)

struct boot_sync_file {
    const char* path;
    uint32_t    done_len;     /* contents last found/made identical on disk */
    uint32_t    done_sum;
    int         done_valid;
};

static struct boot_sync_file boot_sync_files[] = {
    { "/boot/grub/grub.cfg",   0, 0, 0 },
    { "/boot/tanja/tanja.cfg", 0, 0, 0 },
};

static struct iso_vol boot_vol;
static int boot_vol_state = 0;          /* 0 not tried yet, 1 open, -1 none */

static uint32_t bootsync_sum(const uint8_t* d, uint32_t n) {
    uint32_t h = 2166136261u, i;
    for (i = 0; i < n; i++) { h ^= d[i]; h *= 16777619u; }
    return h;
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

static void bootsync_one(struct boot_sync_file* f) {
    static uint8_t want[BOOTSYNC_BUF + 1];
    static uint8_t have[BOOTSYNC_BUF + 1];
    uint32_t want_len, have_len = 0, sum, cap = 0;
    struct iso_node node;
    int rc;

    if (!fs_file_exists(f->path)) return;
    want_len = fs_get_file_size(f->path);
    if (want_len == 0) return;                 /* never leave GRUB an empty menu */

    if (want_len > BOOTSYNC_BUF) {
        /* too big for any config file: warn once per size */
        if (f->done_valid && f->done_len == want_len && f->done_sum == 0) return;
        f->done_valid = 1; f->done_len = want_len; f->done_sum = 0;
        print("[boot] "); print(f->path);
        print(" is too large for the boot image, not written (GRUB will keep the old one)\n");
        return;
    }

    {
        uint32_t sz = BOOTSYNC_BUF;
        if (fs_read_file(f->path, (char*)want, &sz) != 0) return;
        want_len = sz;
    }
    sum = bootsync_sum(want, want_len);
    if (f->done_valid && f->done_len == want_len && f->done_sum == sum) return;

    if (!bootsync_open() || iso_v_lookup(&boot_vol, f->path, &node) != 0 ||
        node.is_dir || node.is_link) {
        /* no boot image on this disk, or the file is not part of it */
        f->done_valid = 1; f->done_len = want_len; f->done_sum = sum;
        return;
    }

    /* already identical on disk? */
    if (node.size == want_len && node.size <= BOOTSYNC_BUF &&
        iso_v_read_range(&boot_vol, &node, 0, have, BOOTSYNC_BUF, &have_len) == 0 &&
        have_len == want_len && bootsync_sum(have, have_len) == sum) {
        f->done_valid = 1; f->done_len = want_len; f->done_sum = sum;
        return;
    }

    rc = iso_v_update_file(&boot_vol, f->path, want, want_len, &cap);
    f->done_valid = 1; f->done_len = want_len; f->done_sum = sum;   /* do not retry the same contents */
    if (rc == 0) {
        print("[boot] "); print(f->path); print(" written to the boot image\n");
    } else if (rc == -2) {
        print("[boot] "); print(f->path);
        print(" does not fit in the boot image (max "); print_dec(cap);
        print(" bytes, it is "); print_dec(want_len);
        print("); GRUB will keep the old one\n");
    } else {
        print("[boot] could not write "); print(f->path); print(" to the boot image\n");
    }
}

static int bootsync_busy = 0;

static void store_sync_boot_files(void) {
    unsigned i;
    if (bootsync_busy) return;
    /* Booted from some other ISO (installer / live media in a drive): /boot/...
       in the tree is that medium's copy, not this disk's. Never copy it
       across. When the overlay IS this disk's own boot image, sync as usual. */
    if (ovl_active_now()) {
        struct vfs_bdev* d = bootsync_store_bdev();
        const char* a = iso_device_name();
        const char* b = d ? d->name : "";
        while (*a && *a == *b) { a++; b++; }
        if (*a || *b || !d) return;     /* overlay is some other medium */
    }
    bootsync_busy = 1;
    for (i = 0; i < sizeof boot_sync_files / sizeof boot_sync_files[0]; i++)
        bootsync_one(&boot_sync_files[i]);
    bootsync_busy = 0;
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

    /* The Storefile is safely on disk: now carry boot config edits over to
       the boot image GRUB actually reads. */
    if (store_enabled)
        store_sync_boot_files();
}

void store_save(void) {
    store_autosave();
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
