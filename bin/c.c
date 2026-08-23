#include "bin.h"

void cmd_c(char* args) {
    extern void print(const char* s);
    extern int fs_read_file(const char* path, char* buffer, uint32_t* size);
    extern void cc_compile_and_run(const char* source, unsigned int len);

    if (!args || !*args) {
        print("Usage: c <file.c>\n");
        return;
    }

    while (*args == ' ') args++;
    char* end = args;
    while (*end) end++;
    end--;
    while (end > args && (*end == ' ' || *end == '\n' || *end == '\r')) { *end = 0; end--; }

    static char buf[1100];
    uint32_t size = 0;
    if (fs_read_file(args, buf, &size) != 0) {
        print("c: cannot read '");
        print(args);
        print("'\n");
        return;
    }
    buf[size] = 0;

    cc_compile_and_run(buf, size);
}
