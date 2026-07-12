#include "cmd.h"

void cmd_touch(char* args) {
    extern void print(const char* s);
    extern int fs_create_file(const char* path);
    extern int fs_file_exists(const char* path);
    
    if (!args || !*args) {
        print("Usage: touch <file>\n");
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
        if (fs_create_file(args) != 0) {
            print("touch: cannot touch '");
            print(args);
            print("': Error\n");
        }
    }
}
