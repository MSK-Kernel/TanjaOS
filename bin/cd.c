#include <stdint.h>
#include <stddef.h>
#include "tanja.h"
#include "utf8.h"

void main(char* args) {

    /* the home directory is per-user (root: /root, others:
     * /home/<name>); the kernel exports the current login's home */
    char home[256];
    tanja_get_home(home, sizeof(home));

    if (!args || !*args) {
        /* like a real shell: bare `cd` goes to the home directory */
        fs_change_directory(home);
        return;
    }

    while (*args == ' ' || *args == '\t') args++;

    if (args[0] == '~' && (args[1] == 0 || args[1] == '/')) {
        if (args[1] == '/' && args[2]) {
            /* ~/subdir -> <home>/subdir */
            char homepath[256];
            int i = 0;
            while (home[i] && i < 250) { homepath[i] = home[i]; i++; }
            int j = 1;
            while (args[j] && i < 255) homepath[i++] = args[j++];
            homepath[i] = 0;
            args = homepath;
        } else {
            args = home;
        }
    }

    char* end = args;
    while (*end) end++;
    end--;
    while (end > args && (*end == ' ' || *end == '\n' || *end == '\r')) {
        *end = 0;
        end--;
    }

    int rc = fs_change_directory(args);
    if (rc != 0) {
        if (rc == -2) {
            /* bash prints exactly this */
            print("cd: ");
            print(args);
            print(": Permission denied\n");
        } else {
            print("cd: ");
            print(args);
            print(": No such file or directory\n");
        }
    }
}
