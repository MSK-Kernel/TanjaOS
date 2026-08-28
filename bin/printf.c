#include "bin.h"

// printf "format" [args...]
//
// A small printf: %s, %d, %c and %% in the format string, plus the
// backslash escapes \n \t \\ \" \' . Unlike `echo`, printf never adds
// a trailing newline on its own - that's what lets
// `printf "Type something: "` leave the cursor sitting on the same
// line, ready for a `read` right after it.
//
// The shell here hands commands a single args string rather than an
// argv array, so the format and each substitution argument are read
// as whitespace-separated words (quote a word to include spaces in it).

static char* skip_spaces(char* s) {
    while (*s == ' ') s++;
    return s;
}

// Reads one word starting at *pp (quoted or not), null-terminates it
// in place, advances *pp past it, and returns it - or 0 if there
// isn't one left.
static char* next_word(char** pp) {
    char* p = skip_spaces(*pp);
    if (!*p) { *pp = p; return 0; }

    char* word;
    if (*p == '"' || *p == '\'') {
        char quote = *p;
        p++;
        word = p;
        while (*p && *p != quote) p++;
        if (*p) { *p = 0; p++; }
    } else {
        word = p;
        while (*p && *p != ' ') p++;
        if (*p) { *p = 0; p++; }
    }

    *pp = p;
    return word;
}

void cmd_printf(char* args) {
    if (!args) args = "";

    char* rest = args;
    char* fmt = next_word(&rest);

    if (!fmt) {
        print("Usage: printf \"format\" [args...]\n");
        return;
    }

    char* p = fmt;
    while (*p) {

        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
                case 'n': putc('\n'); break;
                case 't': putc('\t'); break;
                case '\\': putc('\\'); break;
                case '"': putc('"'); break;
                case '\'': putc('\''); break;
                default: putc(*p); break;
            }
            p++;
            continue;
        }

        if (*p == '%' && p[1]) {
            p++;

            if (*p == '%') { putc('%'); p++; continue; }

            char* arg = next_word(&rest);

            if (*p == 's') {
                print(arg ? arg : "");
            } else if (*p == 'd') {
                // Numeric args already arrive as decimal text from the
                // shell, so there's nothing to reformat - just print
                // it (or 0 if the argument was left out).
                print(arg ? arg : "0");
            } else if (*p == 'c') {
                if (arg && *arg) putc(*arg);
            } else {
                // Unknown conversion - print it literally rather than
                // silently swallowing a typo.
                putc('%');
                putc(*p);
            }

            p++;
            continue;
        }

        putc(*p);
        p++;
    }
}
