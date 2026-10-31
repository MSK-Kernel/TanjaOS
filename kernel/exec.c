#include <stdint.h>
#include "../include/fs.h"

extern void print(const char* s);
extern void execute_command(const char* cmd_line);
extern int elf_run(const uint8_t* image, uint32_t size, const char* path, const char* args);
extern int elf_is_elf32(const uint8_t* image, uint32_t size);
extern int linux_run(const uint8_t* image, uint32_t size,
                     const char* path, const char* args);

/* Depth 0 must hold the largest Linux ELF binaries (>= 4 MiB per the
 * project requirement). Nested execs (a script running a script, an
 * applet spawning another applet) are always small, so depths 1..3
 * get compact 1 MiB buffers - this keeps 4 execution levels without
 * reserving 4 x 4 MiB = 16 MiB of BSS. */
#define EXEC_BUFFER_TOP   (4u * 1024u * 1024 + 1)
#define EXEC_BUFFER_NEST  (1u * 1024u * 1024 + 1)

/* exec_file re-enters itself: a script line can run a /bin applet,
 * which is itself loaded by exec_file, and scripts can call other
 * scripts. One shared buffer would make the nested call wipe the
 * script text the outer call is still iterating, so each nesting
 * level gets its own buffer (kernel BSS is plentiful; store.c alone
 * reserves 20MB). */
#define EXEC_MAX_DEPTH 4
static uint8_t exec_buffers[EXEC_MAX_DEPTH][EXEC_BUFFER_NEST];
static uint8_t exec_buffer_top[EXEC_BUFFER_TOP];
static int exec_depth = 0;

static int exec_file_at(const char* path, const char* args,
                        uint8_t* exec_buffer, uint32_t exec_cap)
{
    if (!path || !path[0]) {
        print("exec: missing file\n");
        return -1;
    }

    uint32_t size = exec_cap;
    const uint8_t* image = exec_buffer;

    /* Prefer a zero-copy view of the file. Large packages (Doom is
     * ~26 MiB) do not fit in the 4 MiB exec buffer; reading them
     * truncated the ELF and failed with "section table out of bounds". */
    {
        uint32_t fsz = 0;
        const char* ptr = fs_file_data_ptr(path, &fsz);
        if (ptr && fsz > 0) {
            image = (const uint8_t*)ptr;
            size = fsz;
        } else if (fs_read_file(path, (char*)exec_buffer, &size) != 0) {
            print("exec: file not found: ");
            print(path);
            print("\n");
            return -1;
        }
    }

    /* Real ELF32: relocatable objects run through the TanjaOS program
 * driver; EXEC images are real Linux binaries - run them through
 * the Linux compat layer (kernel/linux.c). */
    if (elf_is_elf32(image, size)) {
        uint16_t e_type = *(uint16_t*)(image + 16);
        uint16_t e_machine = *(uint16_t*)(image + 18);
        if (e_machine == 3 /* EM_386 */ &&
            (e_type == 2 /* ET_EXEC */ ||
             (e_type == 3 /* ET_DYN: runnable if it is an executable,
 * i.e. has an entry point or an interpreter */))) {
            uint32_t phoff = *(uint32_t*)(image + 28);
            uint16_t phentsize = *(uint16_t*)(image + 42);
            uint16_t phnum = *(uint16_t*)(image + 44);
            uint32_t e_entry = *(uint32_t*)(image + 24);
            int runnable = (e_type == 2) || (e_entry != 0);
            int pi;
            for (pi = 0; pi < phnum && !runnable; pi++) {
                uint32_t p_type = *(uint32_t*)(image + phoff
                                              + pi * phentsize);
                if (p_type == 3 /* PT_INTERP */) runnable = 1;
            }
            if (runnable)
                return linux_run(image, size, path, args);
        }
        return elf_run(image, size, path, args);
    }

    /* Text files are shell scripts. Execute one command per line.
     * Zero-copy images must not be mutated; use a local scan. */
    if (image == exec_buffer)
        exec_buffer[size] = 0;

    char line[256];
    int pos = 0;

    for (uint32_t i = 0; ; i++) {
        if (image[i] == '\n' || image[i] == 0 || i >= size) {
            line[pos] = 0;

            /* Ignore blank lines and simple # comments. */
            int p = 0;
            while (line[p] == ' ' || line[p] == '\t') p++;
            if (line[p] && line[p] != '#')
                execute_command(line);

            pos = 0;
            if (i >= size || image[i] == 0)
                break;
            continue;
        }

        if (pos < 255)
            line[pos++] = (char)image[i];
    }

    return 0;
}

int exec_file(const char* path, const char* args)
{
    int r;
    if (exec_depth >= EXEC_MAX_DEPTH) {
        print("exec: nesting too deep\n");
        return -1;
    }
    uint8_t* buf; uint32_t cap;
    if (exec_depth == 0) { buf = exec_buffer_top;   cap = sizeof exec_buffer_top; }
    else                 { buf = exec_buffers[exec_depth - 1]; cap = sizeof exec_buffers[0]; }
    exec_depth++;
    r = exec_file_at(path, args, buf, cap);
    exec_depth--;
    return r;
}
