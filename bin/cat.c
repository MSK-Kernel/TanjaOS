#include <stdint.h>
#include <stddef.h>
#include "tanja.h"
#include "utf8.h"

/* Quote-aware tokenizer, same rules as rm/cp. */
static int next_arg(char **pp, char *out, int max) {
    char *p=*pp; int n=0, quote=0; char q=0;
    while (*p==' '||*p=='\t') p++; if(!*p){*pp=p;return 0;}
    while(*p){
        if(!quote&&(*p==' '||*p=='\t'))break;
        if((*p=='"'||*p=='\'')&&(!quote||*p==q)){if(!quote){quote=1;q=*p;}else quote=0;p++;continue;}
        if(*p=='\\'&&p[1]){if(n<max-1)out[n++]=p[1];p+=2;continue;}
        if(n<max-1)out[n++]=*p; p++;
    }
    out[n]=0;*pp=p;return n>0;
}

/* Print one file: streamed in 4 KiB chunks so any file size the
 * filesystem stores works. Returns 0 on success, -1 if missing. */
static int cat_one(const char* args) {

    if (!fs_file_exists(args)) {
        /* unreadable files exist but cannot be read (GNU cat reports
 * the same message for both) */
        int mode = fs_get_mode(args);
        if (mode < 0) {
            print("cat: ");
            print(args);
            print(": No such file or directory\n");
        } else {
            print("cat: ");
            print(args);
            print(": Permission denied\n");
        }
        return -1;
    }

    /* Stream the file in chunks instead of copying the whole file into a
 * fixed 4096-byte buffer. This means cat works for every file size
 * the filesystem can store.
     */
    static char buffer[4096];
    uint32_t offset = 0;
    uint32_t got = 0;
    uint32_t total = fs_get_file_size(args);

    while (offset < total) {
        uint32_t want = total - offset;
        if (want > sizeof(buffer)) want = sizeof(buffer);

        if (fs_read_file_range(args, offset, buffer, want, &got) != 0)
            break;

        /* Bulk output: print_n() updates the VGA hardware cursor once per
 * 4 KiB chunk instead of once per character. */
        print_n(buffer, got);

        if (got == 0)
            break;
        offset += got;
    }

    if (total > 0) {
        char last;
        uint32_t last_size = 0;
        if (fs_read_file_range(args, total - 1, &last, 1, &last_size) == 0 &&
            last_size == 1 && last != '\n')
            console_putc('\n');
    }

    return 0;
}

void main(char* args) {

    if (!args || !*args) {
        print("Usage: cat <file>...\n");
        return;
    }

    /* Multiple files, one after another, like real cat - which is what
 * makes `cat *.c` (wildcard-expanded by the shell) useful. */
    char* p = args;
    char fn[256];
    int nfiles = 0;

    while (next_arg(&p, fn, sizeof(fn))) {
        cat_one(fn);
        nfiles++;
    }

    if (!nfiles)
        print("Usage: cat <file>...\n");
}
