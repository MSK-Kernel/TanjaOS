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

/* Kernel export-table introspection (kernel/elf.c) */
extern int tanja_export_count(void);
extern const char* tanja_export_name(int i);

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* ============================================================
 * post-compile check: every external symbol a program references
 * must be a kernel export.  Reporting it HERE turns "elf: undefined
 * symbol" run-time crashes into compile-time errors.
 * ============================================================ */

/* minimal ELF32 structures (no elf.h in the freestanding build) */
typedef struct {
    unsigned char e_ident[16];
    unsigned short e_type, e_machine;
    unsigned int e_version, e_entry, e_phoff, e_shoff, e_flags;
    unsigned short e_ehsize, e_phentsize, e_phnum;
    unsigned short e_shentsize, e_shnum, e_shstrndx;
} Elf32_Ehdr_;
typedef struct {
    unsigned int sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size;
    unsigned int sh_link, sh_info, sh_addralign, sh_entsize;
} Elf32_Shdr_;
typedef struct {
    unsigned int st_name, st_value, st_size;
    unsigned char st_info, st_other;
    unsigned short st_shndx;
} Elf32_Sym_;

#define SHN_UNDEF_ 0
#define SHT_SYMTAB_ 2

static int sym_is_exported(const char *name)
{
    int n = tanja_export_count();
    int i;
    for (i = 0; i < n; i++) {
        if (streq(tanja_export_name(i), name))
            return 1;
    }
    return 0;
}

/* check one produced object file.  Returns 1 if all external
 * symbols resolve against the kernel export table. */
static int check_undef(const char *out_path)
{
    static const char *missed[96];   /* dedup: unique missing names */
    int nmiss = 0;
    uint32_t size = 0;
    char *buf;
    Elf32_Ehdr_ *eh;
    unsigned int off;
    int i, ok = 1;

    size = fs_get_file_size(out_path);
    if (size < sizeof(Elf32_Ehdr_) || size > (4u << 20))
        return 1;                     /* can't read: don't block */
    buf = (char *)malloc(size);
    if (!buf)
        return 1;
    if (fs_read_file(out_path, buf, &size) != 0) {
        free(buf);
        return 1;
    }

    eh = (Elf32_Ehdr_ *)buf;
    if (memcmp(eh->e_ident, "\x7f" "ELF", 4) != 0 || eh->e_type != 1) {
        /* not a relocatable object: nothing to check */
        free(buf);
        return 1;
    }
    if (eh->e_shentsize != sizeof(Elf32_Shdr_) || eh->e_shoff == 0 ||
        eh->e_shoff + (unsigned)eh->e_shnum * sizeof(Elf32_Shdr_) > size) {
        free(buf);
        return 1;
    }

    for (off = eh->e_shoff, i = 0; i < eh->e_shnum;
         off += sizeof(Elf32_Shdr_), i++) {
        Elf32_Shdr_ *sh = (Elf32_Shdr_ *)(buf + off);
        Elf32_Shdr_ *strsh;
        const char *str;
        unsigned nsym, s;

        if (sh->sh_type != SHT_SYMTAB_ || sh->sh_entsize != sizeof(Elf32_Sym_))
            continue;
        if (sh->sh_link >= eh->e_shnum)
            continue;
        strsh = (Elf32_Shdr_ *)(buf + eh->e_shoff +
                                sh->sh_link * sizeof(Elf32_Shdr_));
        if (strsh->sh_offset >= size ||
            strsh->sh_offset + strsh->sh_size > size)
            continue;
        str = buf + strsh->sh_offset;
        nsym = sh->sh_size / sizeof(Elf32_Sym_);
        for (s = 0; s < nsym; s++) {
            Elf32_Sym_ *sym = (Elf32_Sym_ *)(buf + sh->sh_offset +
                                             s * sizeof(Elf32_Sym_));
            const char *nm;
            int j, dup = 0;
            if (sym->st_shndx != SHN_UNDEF_ || sym->st_name == 0)
                continue;
            if ((sym->st_info >> 4) == 2)      /* STB_WEAK: binds to 0 */
                continue;
            if (sym->st_name >= strsh->sh_size)
                continue;
            nm = str + sym->st_name;
            if (sym_is_exported(nm))
                continue;
            for (j = 0; j < nmiss; j++)
                if (streq(missed[j], nm)) { dup = 1; break; }
            if (!dup && nmiss < 96)
                missed[nmiss++] = nm;
            ok = 0;
        }
    }

    if (!ok) {
        printf("tcc: error in '%s':\n", out_path);
        for (i = 0; i < nmiss; i++) {
            printf("tcc:   error: undefined symbol '%s'"
                   " (the TanjaOS kernel does not export it;"
                   " the program would fail to run)\n", missed[i]);
        }
        unlink(out_path);
        g_exit_code = 1;
    }
    free(buf);
    return ok;
}

/* default output name for an input: path with extension -> .o
 * (this is how real tcc names default outputs) */
static void default_output(const char *in, char *out, int cap)
{
    const char *dot = 0, *p = in;
    int n;
    while (*p) { if (*p == '.') dot = p; p++; }
    if (dot)
        n = dot - in;
    else
        n = strlen(in);
    if (n > cap - 3) n = cap - 3;
    memcpy(out, in, n);
    out[n] = '.';
    out[n + 1] = 'o';
    out[n + 2] = 0;
}

static int has_c_extension(const char *f)
{
    size_t len = strlen(f);
    if (len < 2) return 0;
    if (f[len - 2] == '.' && f[len - 1] == 'c') return 1;
    if (f[len - 2] == '.' && f[len - 1] == 'i') return 1;
    if (f[len - 2] == '.' && f[len - 1] == 's') return 1;
    if (f[len - 2] == '.' && f[len - 1] == 'S') return 1;
    if (len >= 4 && f[len - 4] == '.' && f[len - 3] == 'a' &&
        f[len - 2] == 's' && f[len - 1] == 'm') return 1;
    return 0;
}

void main(char *args)
{
    /* room for two inserted flags (-c, -Werror) plus the NULL */
    static char *argv[128];
    static char buf[1024];
    static char outbuf[256];
    static const char *inputs[64];   /* pointers into buf: stable */
    int ninputs = 0;
    int argc = 1;
    int i, o_idx = -1, mode_flag = 0, mode_nobin = 0, link_mode, has_input = 0;
    size_t len;

    argv[0] = (char *)"tcc";
    if (args && args[0]) {
        len = strlen(args);
        if (len >= sizeof(buf)) len = sizeof(buf) - 1;
        memcpy(buf, args, len);
        buf[len] = 0;

        char *p = buf;
        while (*p && argc < 124) {
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

    /* first scan: mode / -o / inputs (before any insertion) */
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (streq(a, "-c") || streq(a, "-E") ||
            streq(a, "-S") || streq(a, "-run")) {
            mode_flag = 1;
            if (!streq(a, "-c"))
                mode_nobin = 1;      /* no binary output produced */
        } else if (streq(a, "-o") && i + 1 < argc) {
            o_idx = i + 1;
        } else if (streq(a, "-B") || streq(a, "-I") || streq(a, "-l") ||
                   streq(a, "-L") || streq(a, "-D") || streq(a, "-U") ||
                   streq(a, "-include")) {
            i++;                    /* flag consumes the next token */
        } else if (a[0] != '-' && !a[0]) {
            ;                       /* empty token: ignore */
        } else if (a[0] != '-') {
            has_input = 1;
            if (ninputs < 64 && has_c_extension(a))
                inputs[ninputs++] = a;
        }
    }
    link_mode = has_input && !mode_flag;

    /* remember the -o target (pointer stays valid) */
    const char *o_val = (o_idx > 0 && o_idx < argc) ? argv[o_idx] : 0;
    if (o_val && !o_val[0]) o_val = 0;

    /* insert -Werror (first), then -c if link mode.  -Werror makes
     * every tcc warning fail the compile: on TanjaOS a warning is
     * almost always a run-time bomb (pointer/int mismatch, bad
     * return, ...).  A user who wants lenient behavior can pass
     * -Wno-error afterwards (later flags win). */
    {
        int shift = 1 + (link_mode ? 1 : 0);
        for (i = argc; i >= 1; i--)
            argv[i + shift] = argv[i];
        argv[1] = (char *)"-Werror";
        if (link_mode)
            argv[2] = (char *)"-c";
        argc += shift;
    }

    if (link_mode && !o_val) {
        /* tcc -c foo.c writes foo.o: find the source to say so */
        for (i = 0; i < ninputs; i++) {
            const char *b = inputs[i], *q = inputs[i];
            size_t blen;
            while (*q) { if (*q == '/') b = q + 1; q++; }
            blen = strlen(b);
            if (blen >= 2) {
                snprintf(outbuf, sizeof(outbuf), "%.*s.o", (int)(blen - 2), b);
                break;
            }
        }
    }

    if (__builtin_setjmp(g_exit_buf) == 0)
        tcc_main_real(argc, argv);

    /* compile succeeded: verify the object(s) against the kernel
     * export table so a missing symbol is reported HERE, not when
     * the program fails to load at run time. */
    if (g_exit_code == 0 && !mode_nobin && (o_val || ninputs > 0)) {
        if (o_val) {
            check_undef(o_val);
        } else {
            static char oname[256];
            for (i = 0; i < ninputs; i++) {
                default_output(inputs[i], oname, sizeof(oname));
                check_undef(oname);
            }
        }
    }
}
