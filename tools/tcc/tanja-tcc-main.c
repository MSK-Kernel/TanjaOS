#include "tanja-libc.h"

int tcc_main_real(int argc, char **argv);

/* __builtin_setjmp saves five words (frame, stack, target PC); the
 * buffer just has to be writable memory that outlives the call. */
static void *g_exit_buf[5];
int g_exit_code = 0;

void exit(int code)
{
    g_exit_code = code;
    __builtin_longjmp(g_exit_buf, 1);
}

void _exit(int code)
{
    exit(code);
}

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

void main(char *args)
{
    /* room for one inserted "-c" plus the NULL terminator */
    static char *argv[128];
    static char buf[1024];
    static char outbuf[256];
    int argc = 1;
    int i, o_idx = -1, mode_flag = 0, link_mode, has_input = 0;
    size_t len;

    argv[0] = (char *)"tcc";
    if (args && args[0]) {
        len = strlen(args);
        if (len >= sizeof(buf)) len = sizeof(buf) - 1;
        memcpy(buf, args, len);
        buf[len] = 0;

        char *p = buf;
        while (*p && argc < 126) {
            char *start;
            while (*p == ' ' || *p == '\t') p++;
            if (!*p) break;
            start = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            if (*p) *p++ = 0;
            argv[argc++] = start;
        }
    }
    argv[argc] = (char *)0;

    /* --- TanjaOS: no linker, only compile-to-object ------------
     * tcc with no -c/-E/-S/-run and an input file means "link an
     * executable", which needs crt1.o and a linker TanjaOS doesn't
     * have. Detect that case (skipping values of flags like -I/-D/-o)
     * so we can downgrade it below. Information-only runs (`tcc -v`,
     * `tcc -print-search-dirs`, ...) have no input file and pass
     * through untouched. */
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (streq(a, "-c") || streq(a, "-E") ||
            streq(a, "-S") || streq(a, "-run"))
            mode_flag = 1;
        else if (streq(a, "-o") && i + 1 < argc)
            o_idx = i + 1;
        else if (streq(a, "-B") || streq(a, "-I") || streq(a, "-l") ||
                 streq(a, "-L") || streq(a, "-D") || streq(a, "-U") ||
                 streq(a, "-include"))
            i++;                    /* flag consumes the next token */
        else if (a[0] != '-' && !a[0])
            ;                       /* empty token: ignore */
        else if (a[0] != '-')
            has_input = 1;          /* an input file: link mode if no -c */
    }
    link_mode = has_input && !mode_flag;

    if (link_mode) {
        /* rewrite argv to: tcc -c [ -o <name> ] <rest>. The -o name is
         * used exactly as given (real tcc keeps -o names verbatim);
         * the kernel runs ELF objects by content, not extension. */
        for (i = argc; i >= 1; i--)
            argv[i + 1] = argv[i];
        argv[1] = (char *)"-c";
        argc++;
        /* the -o value (if any) shifted by one */
        if (o_idx > 0) o_idx++;

        if (o_idx < 0) {
            /* tcc -c foo.c writes foo.o; find the source to say so */
            for (i = 2; i < argc; i++) {
                len = strlen(argv[i]);
                if (argv[i][0] != '-' && len >= 2 &&
                    argv[i][len-2] == '.' && argv[i][len-1] == 'c') {
                    const char *b = argv[i], *q = argv[i];
                    size_t blen;
                    while (*q) { if (*q == '/') b = q + 1; q++; }
                    blen = strlen(b);
                    if (blen >= 2)
                        snprintf(outbuf, sizeof(outbuf), "%.*s.o", (int)(blen-2), b);
                    break;
                }
            }
        }
        /* the "compiled to" note is printed AFTER tcc_main_real()
         * returns, and only if it succeeded - a failed compile
         * must not claim it produced anything */
    }

    if (__builtin_setjmp(g_exit_buf) == 0)
        tcc_main_real(argc, argv);
    /* exit code is in g_exit_code; the shell regains control when
     * this function returns. */
    if (link_mode && g_exit_code == 0) {
        if (o_idx > 0)
            printf("tcc: note: TanjaOS has no linker; compiled to %s\n"
                   "       run it with: ./%s\n", argv[o_idx], argv[o_idx]);
        else if (outbuf[0])
            printf("tcc: note: TanjaOS has no linker; compiled to %s\n"
                   "       run it with: ./%s\n", outbuf, outbuf);
    }
}
