#include "bin.h"

#define READ_BUF_SIZE 256

// read NAME  (or read $NAME - either is accepted)
//
// Blocks for a line of keyboard input, echoing what's typed, and stores
// the result as a shell variable so a later "$NAME" elsewhere in the
// script expands to it. This command is the one exception to normal
// $VAR expansion (see execute_command in kernel.c) - it needs the
// literal variable name it's supposed to fill in, not whatever that
// variable currently holds (usually nothing yet).
void cmd_read(char* args) {
    if (!args) args = "";
    while (*args == ' ') args++;

    char* varname = args;
    if (*varname == '$') varname++;

    // Only one variable name is supported - cut off anything after it.
    char* end = varname;
    while (*end && *end != ' ' && *end != '\t') end++;
    *end = 0;

    if (!*varname) varname = "REPLY"; // same default name real shells use

    char buffer[READ_BUF_SIZE];
    int len = 0;

    while (1) {
        int key = get_key();

        if (key == '\n' || key == KEY_ENTER) {
            putc('\n');
            break;
        }

        if (key == 8 || key == KEY_BACKSPACE) {
            if (len > 0) {
                len--;
                putc('\b');
            }
            continue;
        }

        // Ignore other non-printable/extended keys (arrows etc.) - this
        // is a plain single-line reader, not a full editor.
        if (key >= ' ' && key < 127 && len < READ_BUF_SIZE - 1) {
            buffer[len++] = (char)key;
            putc((char)key);
        }
    }

    buffer[len] = 0;
    set_env(varname, buffer);
}
