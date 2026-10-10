#include <stdint.h>
#include <stddef.h>
#include "tanja.h"
#include "utf8.h"

void main(char* args) {
    
    if (!args || !*args) {
        /* like a real shell: bare `cd` goes to the home directory */
        fs_change_directory("/home");
        return;
    }
    
    while (*args == ' ' || *args == '\t') args++;

    if (args[0] == '~' && (args[1] == 0 || args[1] == '/')) {
        if (args[1] == '/' && args[2]) {
            char homepath[256];
            int i = 0; homepath[i++] = '/'; homepath[i++] = 'h'; homepath[i++] = 'o'; homepath[i++] = 'm'; homepath[i++] = 'e';
            int j = 1; while (args[j] && i < 255) homepath[i++] = args[j++]; homepath[i] = 0;
            args = homepath;
        } else {
            args = "/home";
        }
    }
    
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

