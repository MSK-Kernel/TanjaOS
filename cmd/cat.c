#include "cmd.h"

void cmd_cat(char* args) {
    extern void print(const char* s);
    extern void putc(char c);
    extern int fs_read_file(const char* path, char* buffer, uint32_t* size);
    extern int fs_file_exists(const char* path);
    
    if (!args || !*args) {
        print("Usage: cat <file>\n");
        return;
    }
    
    while (*args == ' ') args++;
    
    char* end = args;
    while (*end) end++;
    end--;
    while (end > args && (*end == ' ' || *end == '\n' || *end == '\r')) {
        *end = 0;
        end--;
    }
    
    if (!fs_file_exists(args)) {
        print("cat: ");
        print(args);
        print(": No such file or directory\n");
        return;
    }
    
    char buffer[4096];
    uint32_t size = 0;
    
    if (fs_read_file(args, buffer, &size) == 0) {
        for (uint32_t i = 0; i < size; i++) {
            putc(buffer[i]);
        }
        if (size > 0 && buffer[size-1] != '\n') {
            putc('\n');
        }
    }
}
