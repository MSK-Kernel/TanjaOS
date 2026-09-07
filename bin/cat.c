#include "bin.h"
#include "../include/fs.h"

void cmd_cat(char* args) {
    extern void print(const char* s);
    extern void putc(char c);

    if (!args || !*args) {
        print("Usage: cat <file>\n");
        return;
    }

    while (*args == ' ' || *args == '\t') args++;

    int len = strlen(args);
    while (len > 0 &&
           (args[len - 1] == ' ' || args[len - 1] == '\t' ||
            args[len - 1] == '\n' || args[len - 1] == '\r'))
        args[--len] = 0;

    if (!fs_file_exists(args)) {
        print("cat: ");
        print(args);
        print(": No such file or directory\n");
        return;
    }

    /*
     * Stream the file in chunks instead of copying the whole file into a
     * fixed 4096-byte buffer.  This means cat works for every file size
     * the filesystem can store.
     */
    static char buffer[4096];
    uint32_t offset = 0;
    uint32_t got = 0;
    uint32_t total = fs_get_file_size(args);

    while (offset < total) {
        uint32_t want = total - offset;
        if (want > sizeof(buffer)) want = sizeof(buffer);

        if (fs_read_file_range(args, offset, buffer, want, &got) != 0)
            break;

        for (uint32_t i = 0; i < got; i++)
            putc(buffer[i]);

        if (got == 0)
            break;
        offset += got;
    }

    if (total > 0) {
        char last;
        uint32_t last_size = 0;
        if (fs_read_file_range(args, total - 1, &last, 1, &last_size) == 0 &&
            last_size == 1 && last != '\n')
            putc('\n');
    }
}
