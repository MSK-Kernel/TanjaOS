#include "bin.h"
#include "../include/fs.h"

static int echo_strlen(const char *s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static void echo_copy(char *dst, const char *src, uint32_t n) {
    uint32_t i;
    for (i = 0; i < n; i++) dst[i] = src[i];
    dst[n] = 0;
}

void cmd_echo(char* args) {
    extern void print(const char* s);
    extern void putc(char c);

    static char data[MAX_FILE_SIZE + 1];
    static char old[MAX_FILE_SIZE + 1];

    if (!args || !*args) {
        putc('\n');
        return;
    }

    while (*args == ' ' || *args == '\t') args++;

    /*
     * Find redirection only when it is outside quotes.  The old parser
     * treated every '>' inside the message as redirection and also wrote
     * the file twice.
     */
    char *redirect = 0;
    int quote = 0;
    char quote_ch = 0;
    char *p;
    for (p = args; *p; p++) {
        if ((*p == '"' || *p == '\'') && (p == args || p[-1] != '\\')) {
            if (!quote) {
                quote = 1;
                quote_ch = *p;
            } else if (*p == quote_ch) {
                quote = 0;
            }
        } else if (*p == '>' && !quote) {
            redirect = p;
            break;
        }
    }

    int append = 0;
    char *filename = 0;
    if (redirect) {
        *redirect = 0;
        redirect++;
        if (*redirect == '>') {
            append = 1;
            redirect++;
        }

        while (*redirect == ' ' || *redirect == '\t') redirect++;
        filename = redirect;

        int flen = echo_strlen(filename);
        while (flen > 0 &&
               (filename[flen - 1] == ' ' || filename[flen - 1] == '\t' ||
                filename[flen - 1] == '\n' || filename[flen - 1] == '\r'))
            filename[--flen] = 0;

        if (!*filename) {
            print("echo: missing file after '>'\n");
            return;
        }
    }

    /* Trim whitespace from the message itself. */
    int len = echo_strlen(args);
    while (len > 0 &&
           (args[len - 1] == ' ' || args[len - 1] == '\t' ||
            args[len - 1] == '\n' || args[len - 1] == '\r'))
        args[--len] = 0;

    /* Remove one matching pair of surrounding quotes. */
    if (len >= 2 &&
        ((args[0] == '"' && args[len - 1] == '"') ||
         (args[0] == '\'' && args[len - 1] == '\''))) {
        args[len - 1] = 0;
        args++;
        len -= 2;
    }

    /*
     * Echo should preserve the complete message.  The shell line buffer
     * and MAX_FILE_SIZE are the only practical limits; there is no small
     * 512/4096-byte echo buffer anymore.
     */
    if (!filename) {
        for (p = args; *p; p++) putc(*p);
        putc('\n');
        return;
    }

    if (!append) {
        if (fs_write_file(filename, args, (uint32_t)len) != 0) {
            print("echo: cannot write '");
            print(filename);
            print("': file is too large, filesystem is full, or directory does not exist\n");
        }
        return;
    }

    uint32_t old_size = 0;
    if (fs_file_exists(filename)) {
        if (fs_read_file(filename, old, &old_size) != 0) {
            print("echo: cannot read '");
            print(filename);
            print("'\n");
            return;
        }
    }

    if (old_size + (uint32_t)len > MAX_FILE_SIZE - 1) {
        print("echo: append would make the file too large\n");
        return;
    }

    echo_copy(data, old, old_size);
    uint32_t i;
    for (i = 0; i < (uint32_t)len; i++)
        data[old_size + i] = args[i];
    data[old_size + (uint32_t)len] = 0;

    if (fs_write_file(filename, data, old_size + (uint32_t)len) != 0) {
        print("echo: cannot write '");
        print(filename);
        print("'\n");
    }
}
