/*
 * kernel/doom.c - the `doom` shell command.
 *
 * Doom (ports/doom) is too big for the in-memory filesystem (a file is at
 * most MAX_FILE_SIZE = 320 KiB and the IWAD is megabytes), so `make doom`
 * links both the Doom ELF32 program and the IWAD straight into the kernel
 * image as binary blobs.  This command runs the program through the normal
 * ELF driver and hands it the IWAD via the tanja_wad_* exports.
 *
 * A plain `make` links neither blob; the weak symbols below then resolve
 * to NULL and `doom` just explains how to build it.
 */
#include <stdint.h>

extern void print(const char* s);
extern int elf_run(const uint8_t* image, uint32_t size, const char* args);

extern const uint8_t _binary_doom_elf_start[] __attribute__((weak));
extern const uint8_t _binary_doom_elf_end[]   __attribute__((weak));
extern const uint8_t _binary_doom_wad_start[] __attribute__((weak));
extern const uint8_t _binary_doom_wad_end[]   __attribute__((weak));

uint8_t* tanja_wad_base;
uint32_t tanja_wad_size;

void cmd_doom(char* args)
{
    if (!_binary_doom_elf_start || !_binary_doom_wad_start) {
        print("doom: not built into this kernel - rebuild with 'make doom'\n");
        return;
    }

    tanja_wad_base = (uint8_t*)_binary_doom_wad_start;
    tanja_wad_size = (uint32_t)(_binary_doom_wad_end - _binary_doom_wad_start);

    elf_run(_binary_doom_elf_start,
            (uint32_t)(_binary_doom_elf_end - _binary_doom_elf_start),
            args ? args : "");
}
