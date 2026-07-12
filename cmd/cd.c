#include "cmd.h"

void cmd_cd(char* args) {
    extern void print(const char* s);
    extern int fs_change_directory(const char* path);
    
    if (!args || !*args) {
        fs_change_directory("/");
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
    
    if (fs_change_directory(args) != 0) {
        print("cd: ");
        print(args);
        print(": No such file or directory\n");
    }
}
