#include <stdint.h>
#include <stddef.h>
#include "tanja.h"
#include "utf8.h"

void main(char* args) {
    (void)args;
    
    char path[256];
    fs_get_current_path(path);
    print(path);
    print("\n");
}

