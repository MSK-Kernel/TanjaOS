#include "cmd.h"

void cmd_mkdir(char* args) {
    extern void print(const char* s);
    extern int fs_create_directory(const char* path);
    extern int fs_directory_exists(const char* path);
    
    if (!args || !*args) {
        print("Usage: mkdir <directory>\n");
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
    
    if (fs_directory_exists(args)) {
        print("mkdir: cannot create directory '");
        print(args);
        print("': File exists\n");
        return;
    }
    
    if (fs_create_directory(args) != 0) {
        print("mkdir: cannot create directory '");
        print(args);
        print("': Error\n");
    }
}
