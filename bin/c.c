#include "bin.h"
#include "../include/fs.h"
#include "../include/cc.h"

#define C_SOURCE_BUFFER (MAX_FILE_SIZE + 1)
#define C_BINARY_BUFFER 262144

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

    char output[MAX_PATH];
    int n = 0;
    while (*args && *args != ' ' && *args != '\t' && n < MAX_PATH - 1)
        output[n++] = *args++;
    output[n] = 0;

    if (!output[0]) {
        print("c: missing output file\n");
        return;
    }

    while (*args == ' ' || *args == '\t') args++;

    char input[MAX_PATH];
    n = 0;
    while (*args && *args != ' ' && *args != '\t' && n < MAX_PATH - 1)
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

    if (fs_file_exists(output) || fs_directory_exists(output)) {
        print("c: ");
        print(output);
        print(": File exists\n");
        return;
    }

    static char source[C_SOURCE_BUFFER];
    static uint8_t binary[C_BINARY_BUFFER];

    uint32_t source_size = C_SOURCE_BUFFER;
    if (fs_read_file(input, source, &source_size) != 0) {
        print("c: cannot read '");
        print(input);
        print("'\n");
        return;
    }

    if (source_size >= C_SOURCE_BUFFER) {
        print("c: source file is too large for the filesystem\n");
        return;
    }

    source[source_size] = 0;

    uint32_t binary_size = 0;
    if (cc_compile_to_binary(source, source_size,
                             binary, C_BINARY_BUFFER, &binary_size) != 0)
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
