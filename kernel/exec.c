#include <stdint.h>
#include "../include/fs.h"

extern void print(const char* s);
extern void execute_command(const char* cmd_line);
extern int elf_run(const uint8_t* image, uint32_t size, const char* path, const char* args);
extern int elf_is_elf32(const uint8_t* image, uint32_t size);
extern int linux_run(const uint8_t* image, uint32_t size,
                     const char* path, const char* args);

#define EXEC_BUFFER (MAX_FILE_SIZE + 1)

/* exec_file re-enters itself: a script line can run a /bin applet,
 * which is itself loaded by exec_file, and scripts can call other
 * scripts.  One shared buffer would make the nested call wipe the
 * script text the outer call is still iterating, so each nesting
 * level gets its own buffer (kernel BSS is plentiful; store.c alone
 * reserves 20MB). */
#define EXEC_MAX_DEPTH 4
static uint8_t exec_buffers[EXEC_MAX_DEPTH][EXEC_BUFFER];
static int exec_depth = 0;

static int exec_file_at(const char* path, const char* args,
                        uint8_t* exec_buffer)
{
    if (!path || !path[0]) {
        print("exec: missing file\n");
        return -1;
    }

    uint32_t size = EXEC_BUFFER;

    if (fs_read_file(path, (char*)exec_buffer, &size) != 0) {
        print("exec: file not found: ");
        print(path);
        print("\n");
        return -1;
    }

    /* Real ELF32: relocatable objects run through the TanjaOS program
     * driver; EXEC images are real Linux binaries - run them through
     * the Linux compat layer (kernel/linux.c). */
    if (elf_is_elf32(exec_buffer, size)) {
        uint16_t e_type = *(uint16_t*)(exec_buffer + 16);
        uint16_t e_machine = *(uint16_t*)(exec_buffer + 18);
        if (e_type == 2 /* ET_EXEC */ && e_machine == 3 /* EM_386 */)
            return linux_run(exec_buffer, size, path, args);
        return elf_run(exec_buffer, size, path, args);
    }

    /* Text files are shell scripts. Execute one command per line, just
     * as the old exec command did. (args is ignored for scripts.) */
    exec_buffer[size] = 0;

    char line[256];
    int pos = 0;

    for (uint32_t i = 0; ; i++) {
        if (exec_buffer[i] == '\n' || exec_buffer[i] == 0) {
            line[pos] = 0;

            /* Ignore blank lines and simple # comments. */
            int p = 0;
            while (line[p] == ' ' || line[p] == '\t') p++;
            if (line[p] && line[p] != '#')
                execute_command(line);

            pos = 0;
            if (exec_buffer[i] == 0)
                break;
            continue;
        }

        if (pos < 255)
            line[pos++] = (char)exec_buffer[i];
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
    uint8_t* buf = exec_buffers[exec_depth++];
    r = exec_file_at(path, args, buf);
    exec_depth--;
    return r;
}
