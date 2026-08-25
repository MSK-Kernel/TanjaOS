#include "bin.h"
#include "../include/fs.h"
#include "../include/cc.h"

#define C_BUFFER 3072

/* Parse the intentionally small command-line grammar:
 *   c -o <output> <input.c>
 * This avoids pretending the shell has a POSIX getopt implementation while
 * still behaving much more like a real compiler command. */
void cmd_c(char *args)
{
    if (!args || !*args) {
        print("Usage: c -o <output> <input.c>\n");
        return;
    }

    while (*args == ' ' || *args == '\t') args++;

    if (args[0] != '-' || args[1] != 'o' ||
        (args[2] != ' ' && args[2] != '\t')) {
        print("Usage: c -o <output> <input.c>\n");
        return;
    }

    args += 2;
    while (*args == ' ' || *args == '\t') args++;

    char output[256];
    int n = 0;
    while (*args && *args != ' ' && *args != '\t' && n < 255)
        output[n++] = *args++;
    output[n] = 0;

    if (!output[0]) {
        print("c: missing output file\n");
        return;
    }

    while (*args == ' ' || *args == '\t') args++;

    char input[256];
    n = 0;
    while (*args && *args != ' ' && *args != '\t' && n < 255)
        input[n++] = *args++;
    input[n] = 0;

    if (!input[0]) {
        print("c: missing input file\n");
        return;
    }

    while (*args == ' ' || *args == '\t') args++;
    if (*args) {
        print("c: too many arguments\n");
        return;
    }

    static char source[C_BUFFER];
    static uint8_t binary[C_BUFFER];

    uint32_t source_size = C_BUFFER;
    if (fs_read_file(input, source, &source_size) != 0) {
        print("c: cannot read '");
        print(input);
        print("'\n");
        return;
    }

    if (source_size >= C_BUFFER) {
        print("c: source file is too large\n");
        return;
    }

    source[source_size] = 0;

    uint32_t binary_size = 0;
    if (cc_compile_to_binary(source, source_size,
                             binary, C_BUFFER - 1, &binary_size) != 0)
        return;

    if (fs_write_file(output, (const char*)binary, binary_size) != 0) {
        print("c: cannot write '");
        print(output);
        print("'\n");
        return;
    }

    print("Compiled ");
    print(input);
    print(" -> ");
    print(output);
    print(" (");
    extern void print_dec(uint32_t n);
    print_dec(binary_size);
    print(" bytes)\n");
}
