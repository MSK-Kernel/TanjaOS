#include "cmd.h"

void cmd_pwd(char* args) {
    (void)args;
    extern void print(const char* s);
    extern void fs_get_current_path(char* path);
    
    char path[256];
    fs_get_current_path(path);
    print(path);
    print("\n");
}
