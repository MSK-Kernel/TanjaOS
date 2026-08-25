#include <stdint.h>
#include "../include/store.h"
#include "../include/ata.h"
#include "../include/ahci.h"
#include "../include/multiboot.h"
#include "../include/fs.h"

extern void print(const char* s);
extern void boot_log(const char* msg);

// From kernel.c - packs/unpacks the login+hostname config (see there
// for details) so it survives reboots the same way the filesystem does.
extern uint32_t config_store_size(void);
extern int config_serialize(uint8_t* buf, uint32_t buf_size);
extern int config_deserialize(const uint8_t* buf, uint32_t buf_size);

// The store region's LBA is *not* a fixed constant — it's computed at
// boot from the disk's own reported size (see store_init below) and
// placed near the very end of the disk. This matters a lot for the
// common case of `dd`-ing an ISO straight onto disk.img and
// booting with a single `-hda`: The bootloader, the kernel, and the Storefile
// module all live at the *start* of that same disk, so a fixed early
// LBA (like the old hardcoded 2048) can land inside the ISO9660
// content and get silently corrupted the first time we write. Writing
// near the end instead only requires that disk.img has spare space
// past whatever was written there (e.g. `qemu-img create disk.img 64M`
// and then `dd if=tanjaos.iso of=disk.img conv=notrunc` — NOT plain
// `dd if=tanjaos.iso of=disk.img`, which would truncate disk.img down
// to the ISO's exact size and leave no room at all).
#define STORE_RESERVED_TAIL_SECTORS 16
#define STORE_FALLBACK_LBA          2048

static uint32_t store_lba = STORE_FALLBACK_LBA;

// Sized generously above fs_store_size() (checked at runtime below) so
// bumping MAX_F/MAX_D in fs.c doesn't silently overflow this buffer.
#define STORE_BUF_SECTORS 255
#define STORE_BUF_BYTES   (STORE_BUF_SECTORS * 512)

static uint8_t store_buf[STORE_BUF_BYTES];
static int store_enabled = 0;
static uint32_t store_sectors = 0;
static uint32_t store_fs_need = 0;
static uint32_t store_cfg_need = 0;
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
    if (backend == BACKEND_ATA) return ata_read_sectors(store_channel, store_drive, lba, (uint8_t)count, buf);
    if (backend == BACKEND_AHCI) return ahci_read_sectors((uint64_t)lba, (uint16_t)count, buf);
    return -1;
}

static int store_backend_write(uint32_t lba, uint32_t count, const void* buf) {
    if (backend == BACKEND_ATA) return ata_write_sectors(store_channel, store_drive, lba, (uint8_t)count, buf);
    if (backend == BACKEND_AHCI) return ahci_write_sectors((uint64_t)lba, (uint16_t)count, buf);
    return -1;
}

void store_autosave(void) {
    if (!store_enabled) return;
    if (fs_serialize(store_buf, store_fs_need) != 0) return;
    config_serialize(store_buf + store_fs_need, store_cfg_need);
    store_backend_write(store_lba, store_sectors, store_buf);
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

void store_init(uint32_t mb_magic, uint32_t mb_addr) {
    store_fs_need = fs_store_size();
    store_cfg_need = config_store_size();
    uint32_t need = store_fs_need + store_cfg_need;
    store_sectors = bytes_to_sectors(need);

    if (need > sizeof(store_buf) || store_sectors > 255) {
        boot_log("Storefile: image too large, running from RAM");
        fs_init();
        return;
    }

    ata_init();

    // Scan all 4 legacy IDE slots and use the first real (non-ATAPI)
    // disk we find. This matters because different VM software puts
    // the CD/ISO and the "real" disk in different slots by default -
    // some put the boot CD on the primary channel and the hard disk on
    // the secondary, others do it the other way round.
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

    // Nothing on legacy IDE - try AHCI. This is what actually makes
    // persistence work on hardware where the BIOS/UEFI only offers
    // AHCI mode, no legacy compatibility option (common on anything
    // reasonably modern).
    if (!found && ahci_init() == 0) {
        backend = BACKEND_AHCI;
        disk_sectors = ahci_get_sector_count();
        found = 1;
        boot_log("Storefile: using AHCI drive");
    }

    if (!found) {
        boot_log("Storefile: no usable disk found (checked legacy IDE and AHCI), running from RAM");
        fs_init();
        return;
    }

    if (backend == BACKEND_ATA) log_slot(store_channel, store_drive);

    // Place the store region near the end of the disk instead of a
    // fixed early offset, so it can't collide with GRUB/kernel/ISO9660
    // content living at the start of the same disk (the common case
    // when booting a single `-hda disk.img` with the ISO written
    // directly onto it). Requires disk.img to have spare space past
    // whatever was written there — see the comment above.
    if (disk_sectors > (uint64_t)(store_sectors + STORE_RESERVED_TAIL_SECTORS + 32)) {
        store_lba = (uint32_t)(disk_sectors - store_sectors - STORE_RESERVED_TAIL_SECTORS);
    } else {
        // Disk is too small to safely reserve tail space (e.g. a tiny
        // test image). Fall back to the old fixed offset and hope for
        // the best rather than refusing to run at all.
        store_lba = STORE_FALLBACK_LBA;
        boot_log("Storefile: disk too small to reserve tail space safely");
    }

    // First, see if there's already a valid saved image on disk from a
    // previous boot.
    if (store_backend_read(store_lba, store_sectors, store_buf) == 0 &&
        fs_deserialize(store_buf, store_fs_need) == 0) {
        store_enabled = 1;
        // Config is appended right after the fs data in the same
        // region. Its own validity was already implied by the fs
        // magic check above; if this is an older disk written before
        // config was included, these bytes are just whatever was on
        // disk (typically zero), which safely means "run the setup
        // wizard once more" rather than anything worse.
        config_deserialize(store_buf + store_fs_need, store_cfg_need);
        boot_log("Storefile: loaded saved state from disk");
        return;
    }

    // Nothing usable on disk yet. Start from a clean filesystem, then
    // see if a "module /boot/Storefile" was handed to us to seed it.
    fs_init();

    if (mb_magic == MULTIBOOT_BOOTLOADER_MAGIC && mb_addr) {
        multiboot_info_t* mbi = (multiboot_info_t*)(uintptr_t)mb_addr;
        if ((mbi->flags & MULTIBOOT_FLAG_MODS) && mbi->mods_count > 0) {
            multiboot_module_t* mods =
                (multiboot_module_t*)(uintptr_t)mbi->mods_addr;
            uint32_t mod_start = mods[0].mod_start;
            uint32_t mod_end   = mods[0].mod_end;
            uint32_t mod_size  = mod_end - mod_start;

            // The Storefile module only ever seeds filesystem content,
            // not login config, so it only needs to cover fs_store_size().
            if (mod_size >= store_fs_need) {
                if (fs_deserialize((const uint8_t*)(uintptr_t)mod_start,
                                   mod_size) == 0) {
                    boot_log("Storefile: seeded state from boot module");
                } else {
                    boot_log("Storefile: boot module wasn't a valid image, starting empty");
                }
            } else {
                boot_log("Storefile: boot module too small, starting empty");
            }
        }
    }

    // Write out whatever we ended up with so the next boot finds it.
    store_enabled = 1;
    store_autosave();
    boot_log("Storefile: persistence enabled, initial state written to disk");
}
