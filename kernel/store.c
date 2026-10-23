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
/* Must cover STORE_HEADER_BYTES + fs_store_size() + config_store_size().
 * File contents now use an 8 MiB on-RAM pool, so a 9 MiB Storefile staging
 * buffer is enough without reserving the old 32+ MiB image. */
#define STORE_BUF_SECTORS 18432
#define STORE_BUF_BYTES   (STORE_BUF_SECTORS * 512)

static uint8_t store_buf[STORE_BUF_BYTES];
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

static void store_write_header(uint32_t payload_bytes) {
    store_buf[0] = 'T'; store_buf[1] = 'J';
    store_buf[2] = 'S'; store_buf[3] = '1';
    /* bytes 4..7 = payload size after this 16-byte header */
    le32_put(&store_buf[4], payload_bytes);
    le32_put(&store_buf[8],  TANJA_BUILD_ID_LO);
    le32_put(&store_buf[12], TANJA_BUILD_ID_HI);
}

static int store_header_valid(void) {
    return store_buf[0] == 'T' && store_buf[1] == 'J'
        && store_buf[2] == 'S' && store_buf[3] == '1';
}

static int store_header_matches_build(void) {
    return le32_at(&store_buf[8])  == (uint32_t)TANJA_BUILD_ID_LO
        && le32_at(&store_buf[12]) == (uint32_t)TANJA_BUILD_ID_HI;
}

void store_autosave(void) {
    if (!store_enabled) return;

    /*
     * Serialize only the live filesystem contents.  This is the main
     * performance fix: mkdir/rm used to rewrite the entire 31 MB maximum
     * filesystem image every time.  v4 writes only metadata + active file
     * bytes, then only the sectors containing that compact image.
     */
    uint32_t fs_bytes = fs_serialized_size();
    uint32_t payload_bytes = fs_bytes + store_cfg_need;
    uint32_t total_bytes = STORE_HEADER_BYTES + payload_bytes;
    uint32_t sectors = bytes_to_sectors(total_bytes);

    if (fs_bytes > store_fs_need ||
        payload_bytes > (store_capacity_sectors * 512u) - STORE_HEADER_BYTES ||
        sectors > store_capacity_sectors ||
        sectors > STORE_BUF_SECTORS) {
        return;
    }

    if (fs_serialize(store_buf + STORE_HEADER_BYTES, fs_bytes) < 0)
        return;
    if (config_serialize(store_buf + STORE_HEADER_BYTES + fs_bytes,
                         store_cfg_need) != 0)
        return;

    store_write_header(payload_bytes);
    store_sectors = sectors;

    if (store_backend_write(store_lba, store_sectors, store_buf) != 0) {
        /* A failed persistence write must never make the kernel continue as
           if the on-disk image were valid. */
        store_enabled = 0;
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

    if (capacity_bytes > sizeof(store_buf) ||
        store_capacity_sectors > STORE_BUF_SECTORS) {
        if (boot_is_livecd(mb_magic, mb_addr)) {
            boot_log("Storefile: This LiveCD session will run from RAM");
            fs_init();
            fs_seed_home();
            return;
        }
        kernel_panic_storage("The Storefile image is too large for the kernel storage buffer.");
    }

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

    /*
     * Reserve enough room for the worst-case filesystem, but do NOT read
     * that whole region at boot.  The first sector contains the exact
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

    /* Read only the tiny header first.  This removes the old ~31 MB boot
       read when the real filesystem is only a few KB/MB. */
    if (store_backend_read(store_lba, 1, store_buf) == 0 &&
        store_header_valid() &&
        store_header_matches_build()) {

        uint32_t payload_bytes = le32_at(&store_buf[4]);
        if (payload_bytes >= 8 + store_cfg_need &&
            payload_bytes <= store_fs_need + store_cfg_need) {
            uint32_t total_bytes = STORE_HEADER_BYTES + payload_bytes;
            store_sectors = bytes_to_sectors(total_bytes);

            if (store_sectors <= store_capacity_sectors &&
                store_sectors <= STORE_BUF_SECTORS &&
                store_backend_read(store_lba, store_sectors, store_buf) == 0) {

                uint32_t fs_bytes = payload_bytes - store_cfg_need;
                if (fs_deserialize(store_buf + STORE_HEADER_BYTES,
                                   fs_bytes) == 0 &&
                    config_deserialize(store_buf + STORE_HEADER_BYTES + fs_bytes,
                                       store_cfg_need) == 0) {
                    store_enabled = 1;
                    boot_log("Storefile: loaded saved state from disk");
                    return;
                }
            }
        }
        boot_log("Storefile: saved image failed validation, starting fresh");
    } else if (store_backend_read(store_lba, 1, store_buf) == 0) {
        if (!store_header_valid())
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
