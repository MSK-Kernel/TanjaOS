#include "../include/fs.h"
#include "cmd.h"
#include <stdint.h>

#define BUFFER_SIZE 4096


static int contains(char *line, char *pattern)
{
    int i;
    int j;

    for(i = 0; line[i]; i++)
    {
        j = 0;

        while(pattern[j] &&
              line[i + j] &&
              line[i + j] == pattern[j])
        {
            j++;
        }

        if(pattern[j] == 0)
            return 1;
    }

    return 0;
}


void cmd_grep(char *args)
{
    if(!args || !args[0])
    {
        print("Usage: grep <text> <file>\n");
        return;
    }


    char pattern[128];
    char filename[256];

    int i = 0;
    int p = 0;


    // Skip spaces
    while(args[i] == ' ')
        i++;


    // Read pattern
    if(args[i] == '"' || args[i] == '\'')
    {
        char quote = args[i];
        i++;

        while(args[i] && args[i] != quote)
        {
            if(p < 127)
                pattern[p++] = args[i];

            i++;
        }

        if(args[i] == quote)
            i++;
    }
    else
    {
        while(args[i] && args[i] != ' ')
        {
            if(p < 127)
                pattern[p++] = args[i];

            i++;
        }
    }

    pattern[p] = 0;


    // Skip spaces before filename
    while(args[i] == ' ')
        i++;


    int f = 0;

    while(args[i])
    {
        if(f < 255)
            filename[f++] = args[i];

        i++;
    }

    filename[f] = 0;


    if(filename[0] == 0)
    {
        print("Usage: grep <text> <file>\n");
        return;
    }


    char buffer[BUFFER_SIZE];

    uint32_t size = BUFFER_SIZE - 1;


    if(fs_read_file(filename, buffer, &size) < 0)
    {
        print("grep: file not found\n");
        return;
    }


    buffer[size] = 0;


    char line[256];

    int line_pos = 0;


    for(uint32_t x = 0; x <= size; x++)
    {
        if(buffer[x] == '\n' || buffer[x] == 0)
        {
            line[line_pos] = 0;


            if(contains(line, pattern))
            {
                print(line);
                print("\n");
            }


            line_pos = 0;
        }
        else
        {
            if(line_pos < 255)
                line[line_pos++] = buffer[x];
        }
    }
}
