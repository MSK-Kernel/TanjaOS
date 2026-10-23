#include <stdint.h>
#include "../include/store.h"
#include "../include/ata.h"
#include "../include/ahci.h"
#include "../include/multiboot.h"
#include "../include/fs.h"
#include "../include/build_id.h"

extern void print(const char* s);
extern void boot_log(const char* msg);

// From kernel.c - packs/unpacks the login+hostname config (see there
// for details) so it survives reboots the same way the filesystem does.
extern uint32_t config_store_size(void);
extern uint32_t fs_serialized_size(void);
extern int config_serialize(uint8_t* buf, uint32_t buf_size);
extern int config_deserialize(const uint8_t* buf, uint32_t buf_size);

#define STORE_RESERVED_TAIL_SECTORS 16
#define STORE_FALLBACK_LBA          2048

// Minimum LBA the persistent region is allowed to start at. The installer
// dd's the boot image (GRUB + kernel + config) onto the front of the disk
// with no partition table separating it from anything else, so store_lba
// (computed backward from the end of the disk) must land safely past that
// image or every autosave silently overwrites GRUB's own files. 32768
// sectors = 16 MiB, comfortably past the ~6.3 MB livecd image with room
// to grow.
#define STORE_FRONT_RESERVE_SECTORS 32768

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

static int boot_is_livecd(uint32_t mb_magic, uint32_t mb_addr) {
    /* A normal LiveCD intentionally has no Storefile module: it is the
       RAM-only boot mode. Install media carries a Storefile module, so a
       missing/undersized target disk still fails instead of silently
       continuing from RAM. If a bootloader explicitly labels the boot
       command/module as live/livecd, honor that too. */
    if (mb_magic != MULTIBOOT_BOOTLOADER_MAGIC || mb_addr == 0)
        return 0;

    multiboot_info_t* mbi = (multiboot_info_t*)(uintptr_t)mb_addr;

    /* Multiboot1 boot_device stores the BIOS drive number in its top byte.
       0xE0-0xFF are optical/CD-ROM BIOS drives.  When GRUB boots the
       kernel directly from a LiveCD ISO, this lets us identify the CD boot
       even when GRUB did not pass a "live" command-line/module string. */
    if (mbi->flags & 0x00000002u) {
        uint8_t bios_drive = (uint8_t)(mbi->boot_device >> 24);
        if (bios_drive >= 0xE0u)
            return 1;
    }

    if (mbi->cmdline) {
        const char *s = (const char*)(uintptr_t)mbi->cmdline;
        while (*s) {
            if ((s[0] == 'l' || s[0] == 'L') &&
                (s[1] == 'i' || s[1] == 'I') &&
                (s[2] == 'v' || s[2] == 'V') &&
                (s[3] == 'e' || s[3] == 'E'))
                return 1;
            s++;
        }
    }

    if ((mbi->flags & MULTIBOOT_FLAG_MODS) && mbi->mods_count > 0) {
        multiboot_module_t* mods =
            (multiboot_module_t*)(uintptr_t)mbi->mods_addr;
        int i;
        for (i = 0; i < (int)mbi->mods_count; i++) {
            const char *s = (const char*)(uintptr_t)mods[i].string;
            if (!s) continue;
            while (*s) {
                if ((s[0] == 'l' || s[0] == 'L') &&
                    (s[1] == 'i' || s[1] == 'I') &&
                    (s[2] == 'v' || s[2] == 'V') &&
                    (s[3] == 'e' || s[3] == 'E'))
                    return 1;
                s++;
            }
        }
        /* Storefile-bearing media are treated as install/persistent media. */
        return 0;
    }

    return 1;
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

    if (!found && ahci_init() == 0) {
        backend = BACKEND_AHCI;
        disk_sectors = ahci_get_sector_count();
        found = 1;
        boot_log("Storefile: using AHCI drive");
    }

    if (!found) {
        if (boot_is_livecd(mb_magic, mb_addr)) {
            boot_log("Storefile: This LiveCD session will run from RAM");
            fs_init();
            fs_seed_home();
            return;
        }
        kernel_panic_storage("No usable persistent disk was found.");
    }

    store_disk_sectors = disk_sectors;
    if (backend == BACKEND_ATA) log_slot(store_channel, store_drive);

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
        kernel_panic_storage("Minimum recommended disk size is 47 MiB");
    }

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
        kernel_panic_storage("Disk too small, minimum recommended disk size is 47 MiB");
    }

    /* Read only the tiny header first: one sector. If it validates,
       the payload streams straight from disk through the staging
       window - no whole-image RAM buffer. */
    if (store_backend_read(store_lba, 1, store_stage) == 0 &&
        store_header_valid_at(store_stage) &&
        store_header_matches_build_at(store_stage)) {

        uint32_t payload_bytes = le32_at(&store_stage[4]);
        if (payload_bytes >= 8 + store_cfg_need &&
            payload_bytes <= store_fs_need + store_cfg_need) {
            uint32_t total_bytes = STORE_HEADER_BYTES + payload_bytes;
            store_sectors = bytes_to_sectors(total_bytes);

            if (store_sectors <= store_capacity_sectors) {
                store_rd_ctx rd;
                uint32_t fs_bytes = payload_bytes - store_cfg_need;
                rd.img_off  = STORE_HEADER_BYTES;
                rd.img_end  = total_bytes;
                rd.win_off  = 0;
                rd.win_len  = 0;

                if (fs_deserialize_fill(&rd, store_rd_fill) == 0 &&
                    store_rd_fill(&rd, store_cfgbuf, store_cfg_need) == 0 &&
                    config_deserialize(store_cfgbuf, store_cfg_need) == 0) {
                    store_enabled = 1;
                    boot_log("Storefile: loaded saved state from disk");
                    return;
                }
            }
        }
        boot_log("Storefile: saved image failed validation, starting fresh");
    } else if (store_backend_read(store_lba, 1, store_stage) == 0) {
        if (!store_header_valid_at(store_stage))
            boot_log("Storefile: on-disk image starting from a fresh condition");
        else
            boot_log("Storefile: saved state is from a new/custom version");
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
