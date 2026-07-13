#include <stdint.h>
#include "cmd.h"
#include "../include/fs.h"

extern void print(const char* s);
extern void execute_command(const char* cmd);

#define SCRIPT_BUFFER 4096


void cmd_exec(char* args)
{
    if (!args || !args[0]) {
        print("usage: exec <file>\n");
        return;
    }


    char buffer[SCRIPT_BUFFER];

    uint32_t size = SCRIPT_BUFFER - 1;

    fs_read_file(args, buffer, &size);

    if (size == 0) {
        print("exec: file not found\n");
        return;
    }


    buffer[size] = 0;


    char line[256];
    int pos = 0;


    for (int i = 0; ; i++) {

        if (buffer[i] == '\n' || buffer[i] == 0) {

            line[pos] = 0;


            if (pos > 0)
                execute_command(line);


            pos = 0;


            if (buffer[i] == 0)
                break;


            continue;
        }


        if (pos < 255)
            line[pos++] = buffer[i];
    }
}
