#include "cmd.h"

void cmd_ls(char* args) {
    extern void print(const char* s);
    extern int fs_list_directory(const char* path, char* buffer, uint32_t* size);
    
    char buffer[4096];
    uint32_t size = 0;
    
    if (fs_list_directory(args, buffer, &size) == 0) {
        if (size > 0) {
            print(buffer);
        }
    }
}
