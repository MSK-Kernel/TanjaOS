#include <stdint.h>
#include <stddef.h>
#include "tanja.h"

static int icase;

static int chr_eq(char a, char b)
{
    if(icase)
    {
        if(a >= 'A' && a <= 'Z') a += 32;
        if(b >= 'A' && b <= 'Z') b += 32;
    }
    return a == b;
}

/* does a match start at line[0]? */
static int match_at(const char* line, const char* pat)
{
    int j = 0;

    while(pat[j])
    {
        if(!line[j] || !chr_eq(line[j], pat[j]))
            return 0;
        j++;
    }

    return 1;
}

static int line_matches(const char* line, const char* pat)
{
    int i;

    if(!pat[0])
        return 1;

    for(i = 0; line[i]; i++)
    {
        if(match_at(line + i, pat))
            return 1;
    }

    return 0;
}

/* print a line with every occurrence of pat highlighted in red */
static void print_hi(const char* line, const char* pat)
{
    int i = 0;
    int plen = strlen(pat);

    if(!pat[0])
    {
        print(line);
        return;
    }

    while(line[i])
    {
        if(match_at(line + i, pat))
        {
            for(int j = 0; j < plen; j++)
                putc_color(line[i + j], COLOR_RED);

            i += plen;
        }
        else
        {
            putc(line[i]);
            i++;
        }
    }
}

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

static char pattern[128];
static char files[16][256];

static void show_line(const char* line, const char* pat,
                      int line_nums, int lineno,
                      int invert, int multi, const char* fname)
{
    int found = line_matches(line, pat);

    if((found && !invert) || (!found && invert))
    {
        if(multi) { print(fname); print(":"); }
        if(line_nums) { print_dec(lineno); print(":"); }

        if(found && !invert)
            print_hi(line, pat);
        else
            print(line);

        putc('\n');
    }
}

void main(char* args)
{
    int line_nums = 0, invert = 0, have_pattern = 0, nfiles = 0;
    icase = 0;
    pattern[0] = 0;

    if(!args || !args[0])
    {
        print("Usage: grep [-inv] <text> [file...]\n");
        return;
    }

    char arg[256];
    char* p = args;

    while(next_arg(&p, arg, sizeof(arg)))
    {
        if(!have_pattern && arg[0] == '-' && arg[1] && arg[1] != '-')
        {
            int bad = 0;

            for(int j = 1; arg[j]; j++)
            {
                if(arg[j] == 'i') icase = 1;
                else if(arg[j] == 'n') line_nums = 1;
                else if(arg[j] == 'v') invert = 1;
                else bad = 1;
            }

            if(bad)
            {
                print("grep: invalid option\n");
                print("Usage: grep [-inv] <text> [file...]\n");
                return;
            }

            continue;
        }

        if(!have_pattern)
        {
            int n = 0;
            while(arg[n] && n < 127) { pattern[n] = arg[n]; n++; }
            pattern[n] = 0;
            have_pattern = 1;
            continue;
        }

        if(nfiles < 16)
        {
            int n = 0;
            while(arg[n] && n < 255) { files[nfiles][n] = arg[n]; n++; }
            files[nfiles][n] = 0;
            nfiles++;
        }
    }

    if(!have_pattern)
    {
        print("Usage: grep [-inv] <text> [file...]\n");
        return;
    }

    /* No files: read lines from the keyboard until Ctrl-D,
     * like `grep word` reading stdin in real bash. */
    if(nfiles == 0)
    {
        static char line[512];
        int lineno = 0;

        for(;;)
        {
            read_line(line, sizeof(line));

            int len = strlen(line);
            int eof = (len > 0 && line[len - 1] == 4);
            if(eof) line[len - 1] = 0;

            lineno++;
            show_line(line, pattern, line_nums, lineno, invert, 0, 0);

            if(eof) break;
        }

        return;
    }

    static char buffer[MAX_FILE_SIZE + 1];

    for(int f = 0; f < nfiles; f++)
    {
        uint32_t size = MAX_FILE_SIZE;

        if(fs_read_file(files[f], buffer, &size) < 0)
        {
            print("grep: ");
            print(files[f]);
            print(": No such file or directory\n");
            continue;
        }

        buffer[size] = 0;

        char line[256];
        int line_pos = 0;
        int lineno = 0;

        for(uint32_t x = 0; x <= size; x++)
        {
            if(buffer[x] == '\n' || buffer[x] == 0)
            {
                line[line_pos] = 0;
                lineno++;

                show_line(line, pattern, line_nums, lineno,
                         invert, nfiles > 1, files[f]);

                line_pos = 0;
            }
            else
            {
                if(line_pos < 255)
                    line[line_pos++] = buffer[x];
            }
        }
    }
}
