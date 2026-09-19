#ifndef STORE_H
#define STORE_H

#include <stdint.h>

// Call once from kernel_main with the raw multiboot magic/info pointer
// (whatever the bootloader left in eax/ebx). Figures out whether we're running
// from a real disk, loads any previously saved state from the fixed
// Storefile region, and if this is a first boot, seeds it from the
// "module /boot/Storefile" multiboot module (if one was given). Always
// leaves the filesystem initialized one way or another.
void store_init(uint32_t mb_magic, uint32_t mb_addr);

// Force-write the current filesystem state to disk right now.
void store_save(void);

// Same as store_save(), but silently does nothing if persistence isn't
// active. This is what fs.c calls after every mutation.
void store_autosave(void);

// 1 if a disk-backed Storefile is active (writes are being persisted),
// 0 if we're running from RAM (no disk found, or it was too small).
int store_is_persistent(void);

#endif
