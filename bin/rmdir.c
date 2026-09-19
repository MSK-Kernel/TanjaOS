/* ============================================================
 * TANJA OS BIN COMMAND - rmdir
 * ------------------------------------------------------------
 * A standalone ELF32 user program, compiled by the Makefile with
 * plain gcc -m32 (freestanding) and embedded in the kernel image
 * (via home.tar) as a /bin command at boot.  Replace or add commands by editing
 * or dropping files into bin/.
 * ============================================================ */

#include <stdint.h>
#include <stddef.h>
#include "tanja.h"
#include "utf8.h"

void main(char* args) {
    
    if (!args || !*args) {
        print("Usage: rmdir <directory>\n");
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
    
    if (!fs_directory_exists(args)) {
        print("rmdir: failed to remove '");
        print(args);
        print("': No such file or directory\n");
        return;
    }
    
    int result = fs_delete_directory(args);
    if (result == -2) {
        print("rmdir: failed to remove '");
        print(args);
        print("': Directory not empty\n");
    } else if (result != 0) {
        print("rmdir: failed to remove '");
        print(args);
        print("': Error\n");
    }
}

