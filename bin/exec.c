#include <stdint.h>
#include "bin.h"
#include "../include/fs.h"
#include "../include/cc.h"

#define EXEC_BUFFER 3072

static int is_tanja_binary(const uint8_t *buf, uint32_t size)
{
    return size >= 12 &&
           buf[0] == 'T' && buf[1] == 'J' &&
           buf[2] == 'B' && buf[3] == 'N' &&
           buf[4] == 1;
}

extern void execute_command(const char *cmd);

int exec_file(const char *path)
{
    if (!path || !path[0]) {
        print("exec: missing file\n");
        return -1;
    }

    static uint8_t buffer[EXEC_BUFFER];
    uint32_t size = EXEC_BUFFER;

    if (fs_read_file(path, (char*)buffer, &size) != 0) {
        print("exec: file not found: ");
        print(path);
        print("\n");
        return -1;
    }

    if (is_tanja_binary(buffer, size))
        return cc_execute_binary(buffer, size);

    /* Text files are shell scripts. Execute one command per line, just as
     * the old exec command did. */
    buffer[size] = 0;

    char line[256];
    int pos = 0;

    for (uint32_t i = 0; ; i++) {
        if (buffer[i] == '\n' || buffer[i] == 0) {
            line[pos] = 0;

            /* Ignore blank lines and simple # comments. */
            int p = 0;
            while (line[p] == ' ' || line[p] == '\t') p++;
            if (line[p] && line[p] != '#')
                execute_command(line);

            pos = 0;
            if (buffer[i] == 0)
                break;
            continue;
        }

        if (pos < 255)
            line[pos++] = (char)buffer[i];
    }

    return 0;
}

void cmd_exec(char *args)
{
    if (!args || !args[0]) {
        print("Usage: exec <file>\n");
        return;
    }

    exec_file(args);
}
