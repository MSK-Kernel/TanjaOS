#include <stdint.h>
#include <stddef.h>
#include "tanja.h"

/* export NAME=value   (value may be quoted; may contain spaces) */
void main(char* args) {
    char name[64], value[200];
    int n = 0, v = 0;

    while (*args == ' ') args++;
    while (*args && *args != '=' && *args != ' ' && n < 63) name[n++] = *args++;
    name[n] = 0;
    if (!n || *args != '=') {
        print("usage: export NAME=value\n");
        return;
    }
    args++;
    if ((*args == '"' || *args == '\'')) {
        char q = *args++;
        while (*args && *args != q && v < 199) value[v++] = *args++;
    } else {
        while (*args && v < 199) value[v++] = *args++;
    }
    value[v] = 0;
    set_env(name, value);
}
