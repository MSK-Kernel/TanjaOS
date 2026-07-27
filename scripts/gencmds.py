#!/usr/bin/env python3
import os

commands = {}

commands["help.c"] = r"""#include "cmd.h"

void cmd_help(char* args) {
    (void)args;
    extern void list_commands(void);
    list_commands();
}
"""

commands["reboot.c"] = r"""#include "cmd.h"

void cmd_reboot(char* args) {
    (void)args;
    extern void print(const char* s);
    extern uint8_t inb(uint16_t port);
    extern void outb(uint16_t port, uint8_t val);
    print("Rebooting...\n");
    for (volatile int i = 0; i < 1000000; i++);
    while ((inb(0x64) & 0x02) != 0);
    outb(0x64, 0xFE);
    asm volatile ("lidt 0\n" "int $0");
    while (1);
}
"""

commands["echo.c"] = r"""#include "cmd.h"

void cmd_echo(char* args) {
    extern void print(const char* s);
    extern void putc(char c);
    extern int fs_write_file(const char* path, const char* data, uint32_t size);
    extern int fs_file_exists(const char* path);
    
    if (!args || !*args) {
        putc('\n');
        return;
    }
    
    while (*args == ' ') args++;
    
    // Check for redirection
    char* redirect = args;
    char* filename = 0;
    int append = 0;
    
    while (*redirect) {
        if (*redirect == '>') {
            *redirect = 0;
            redirect++;
            if (*redirect == '>') {
                append = 1;
                redirect++;
            }
            while (*redirect == ' ') redirect++;
            if (*redirect) {
                filename = redirect;
                // Trim trailing spaces from filename
                char* end = filename;
                while (*end) end++;
                end--;
                while (end > filename && (*end == ' ' || *end == '\n' || *end == '\r')) {
                    *end = 0;
                    end--;
                }
            }
            break;
        }
        redirect++;
    }
    
    // Remove trailing spaces from text
    char* end = args;
    while (*end) end++;
    end--;
    while (end > args && *end == ' ') {
        *end = 0;
        end--;
    }
    
    // Handle quotes
    char* text = args;
    if (*text == '"') {
        text++;
        end = text;
        while (*end && *end != '"') end++;
        *end = 0;
    } else if (*text == '\'') {
        text++;
        end = text;
        while (*end && *end != '\'') end++;
        *end = 0;
    }
    
    if (filename) {
        // Write to file
        uint32_t len = 0;
        char* p = text;
        while (*p) { len++; p++; }
        fs_write_file(filename, text, len);
    } else {
        // Print to screen
        char* p = text;
        while (*p) {
            putc(*p);
            p++;
        }
        putc('\n');
    }
}
"""

commands["clear.c"] = r"""#include "cmd.h"

void cmd_clear(char* args) {
    (void)args;
    extern void clear_screen(void);
    clear_screen();
}
"""

commands["mkdir.c"] = r"""#include "cmd.h"

void cmd_mkdir(char* args) {
    extern void print(const char* s);
    extern int fs_create_directory(const char* path);
    extern int fs_directory_exists(const char* path);
    
    if (!args || !*args) {
        print("Usage: mkdir <directory>\n");
        return;
    }
    
    while (*args == ' ') args++;
    
    char* end = args;
    while (*end) end++;
    end--;
    while (end > args && (*end == ' ' || *end == '\n' || *end == '\r')) {
        *end = 0;
        end--;
    }
    
    if (fs_directory_exists(args)) {
        print("mkdir: cannot create directory '");
        print(args);
        print("': File exists\n");
        return;
    }
    
    if (fs_create_directory(args) != 0) {
        print("mkdir: cannot create directory '");
        print(args);
        print("': Error\n");
    }
}
"""

commands["rmdir.c"] = r"""#include "cmd.h"

void cmd_rmdir(char* args) {
    extern void print(const char* s);
    extern int fs_delete_directory(const char* path);
    extern int fs_directory_exists(const char* path);
    
    if (!args || !*args) {
        print("Usage: rmdir <directory>\n");
        return;
    }
    
    while (*args == ' ') args++;
    
    char* end = args;
    while (*end) end++;
    end--;
    while (end > args && (*end == ' ' || *end == '\n' || *end == '\r')) {
        *end = 0;
        end--;
    }
    
    if (!fs_directory_exists(args)) {
        print("rmdir: failed to remove '");
        print(args);
        print("': No such file or directory\n");
        return;
    }
    
    int result = fs_delete_directory(args);
    if (result == -2) {
        print("rmdir: failed to remove '");
        print(args);
        print("': Directory not empty\n");
    } else if (result != 0) {
        print("rmdir: failed to remove '");
        print(args);
        print("': Error\n");
    }
}
"""

commands["touch.c"] = r"""#include "cmd.h"

void cmd_touch(char* args) {
    extern void print(const char* s);
    extern int fs_create_file(const char* path);
    extern int fs_file_exists(const char* path);
    
    if (!args || !*args) {
        print("Usage: touch <file>\n");
        return;
    }
    
    while (*args == ' ') args++;
    
    char* end = args;
    while (*end) end++;
    end--;
    while (end > args && (*end == ' ' || *end == '\n' || *end == '\r')) {
        *end = 0;
        end--;
    }
    
    if (!fs_file_exists(args)) {
        if (fs_create_file(args) != 0) {
            print("touch: cannot touch '");
            print(args);
            print("': Error\n");
        }
    }
}
"""

commands["rm.c"] = r"""#include "cmd.h"

void cmd_rm(char* args) {
    extern void print(const char* s);
    extern int fs_delete_file(const char* path);
    extern int fs_file_exists(const char* path);
    
    if (!args || !*args) {
        print("Usage: rm <file>\n");
        return;
    }
    
    while (*args == ' ') args++;
    
    char* end = args;
    while (*end) end++;
    end--;
    while (end > args && (*end == ' ' || *end == '\n' || *end == '\r')) {
        *end = 0;
        end--;
    }
    
    if (!fs_file_exists(args)) {
        print("rm: cannot remove '");
        print(args);
        print("': No such file or directory\n");
        return;
    }
    
    fs_delete_file(args);
}
"""

commands["cat.c"] = r"""#include "cmd.h"

void cmd_cat(char* args) {
    extern void print(const char* s);
    extern void putc(char c);
    extern int fs_read_file(const char* path, char* buffer, uint32_t* size);
    extern int fs_file_exists(const char* path);
    
    if (!args || !*args) {
        print("Usage: cat <file>\n");
        return;
    }
    
    while (*args == ' ') args++;
    
    char* end = args;
    while (*end) end++;
    end--;
    while (end > args && (*end == ' ' || *end == '\n' || *end == '\r')) {
        *end = 0;
        end--;
    }
    
    if (!fs_file_exists(args)) {
        print("cat: ");
        print(args);
        print(": No such file or directory\n");
        return;
    }
    
    char buffer[4096];
    uint32_t size = 0;
    
    if (fs_read_file(args, buffer, &size) == 0) {
        for (uint32_t i = 0; i < size; i++) {
            putc(buffer[i]);
        }
        if (size > 0 && buffer[size-1] != '\n') {
            putc('\n');
        }
    }
}
"""

commands["ls.c"] = r"""#include "cmd.h"

void cmd_ls(char* args) {
    extern void print(const char* s);
    extern int fs_list_directory(const char* path, char* buffer, uint32_t* size);
    
    char buffer[4096];
    uint32_t size = 0;
    
    if (fs_list_directory(args, buffer, &size) == 0) {
        if (size > 0) {
            print(buffer);
        }
    }
}
"""

commands["pwd.c"] = r"""#include "cmd.h"

void cmd_pwd(char* args) {
    (void)args;
    extern void print(const char* s);
    extern void fs_get_current_path(char* path);
    
    char path[256];
    fs_get_current_path(path);
    print(path);
    print("\n");
}
"""

commands["cd.c"] = r"""#include "cmd.h"

void cmd_cd(char* args) {
    extern void print(const char* s);
    extern int fs_change_directory(const char* path);
    
    if (!args || !*args) {
        fs_change_directory("/");
        return;
    }
    
    while (*args == ' ') args++;
    
    char* end = args;
    while (*end) end++;
    end--;
    while (end > args && (*end == ' ' || *end == '\n' || *end == '\r')) {
        *end = 0;
        end--;
    }
    
    if (fs_change_directory(args) != 0) {
        print("cd: ");
        print(args);
        print(": No such file or directory\n");
    }
}
"""

commands["editor.c"] = r'''
#include "../include/fs.h"
#include <stdint.h>

#define KEY_UP     0x80
#define KEY_DOWN   0x81
#define KEY_LEFT   0x82
#define KEY_RIGHT  0x83
#define KEY_ENTER  0x84
#define KEY_BACKSPACE 0x85

#define CTRL_X 24
#define MAX_TEXT 1024

extern void print(const char*);
extern void clear_screen(void);
extern void putc(char);
extern int get_key(void);

extern int cursor;
extern void sync_cursor(void);

static int strlen_editor(char *s)
{
    int i=0;
    while(s[i]) i++;
    return i;
}

static int get_column(char *text,int pos)
{
    int col=0;

    while(pos>0 && text[pos-1]!='\n')
    {
        col++;
        pos--;
    }

    return col;
}

static int line_start(char *text,int pos)
{
    while(pos>0 && text[pos-1]!='\n')
        pos--;

    return pos;
}

static void draw_editor(char *text,int pos)
{
    clear_screen();

    print("TanjaOS Editor\n");
    print("Ctrl+X = Save & Exit\n");
    print("---------------------\n");

    for(int i=0;text[i];i++)
        putc(text[i]);

    int screen=0;

    for(int i=0;i<pos;i++)
    {
        if(text[i]=='\n')
            screen=((screen/80)+1)*80;
        else
            screen++;
    }

    cursor=(80*3)+screen;
    sync_cursor();
}

void cmd_editor(char *args)
{
    if(!args || !args[0])
    {
        print("Usage: editor <file>\n");
        return;
    }

    char text[MAX_TEXT];
    uint32_t size=0;

    text[0]=0;

    if(fs_file_exists(args))
        fs_read_file(args,text,&size);
    else
        fs_create_file(args);

    int pos=strlen_editor(text);

    while(1)
    {
        draw_editor(text,pos);

        int key=get_key();

        if(key==CTRL_X)
        {
            fs_write_file(args,text,strlen_editor(text));
            print("\n");
            return;
        }

        else if(key==KEY_LEFT)
        {
            if(pos>0)
                pos--;
        }

        else if(key==KEY_RIGHT)
        {
            if(pos<strlen_editor(text))
                pos++;
        }

        else if(key==KEY_UP)
        {
            int col=get_column(text,pos);
            int start=line_start(text,pos);

            if(start>0)
            {
                start--;

                while(start>0 && text[start-1]!='\n')
                    start--;

                pos=start;

                while(col>0 &&
                      text[pos] &&
                      text[pos]!='\n')
                {
                    pos++;
                    col--;
                }
            }
        }

        else if(key==KEY_DOWN)
        {
            int col=get_column(text,pos);
            int len=strlen_editor(text);

            while(pos<len && text[pos]!='\n')
                pos++;

            if(pos<len)
            {
                pos++;

                while(col>0 &&
                      text[pos] &&
                      text[pos]!='\n')
                {
                    pos++;
                    col--;
                }
            }
        }

        else if(key==KEY_ENTER || key=='\n')
        {
            int len=strlen_editor(text);

            if(len<MAX_TEXT-1)
            {
                for(int i=len;i>=pos;i--)
                    text[i+1]=text[i];

                text[pos]='\n';
                pos++;
            }
        }

        else if(key==KEY_BACKSPACE || key==8)
        {
            if(pos>0)
            {
                int len=strlen_editor(text);

                for(int i=pos-1;i<len;i++)
                    text[i]=text[i+1];

                pos--;
            }
        }

        else if(key>=32 && key<=126)
        {
            int len=strlen_editor(text);

            if(len<MAX_TEXT-1)
            {
                for(int i=len;i>=pos;i--)
                    text[i+1]=text[i];

                text[pos]=key;
                pos++;
            }
        }
    }
}
'''
commands["exec.c"] = r"""#include <stdint.h>
#include "cmd.h"
#include "../include/fs.h"

extern void print(const char* s);
extern void execute_command(const char* cmd);

#define SCRIPT_BUFFER 4096


void cmd_exec(char* args)
{
    if (!args || !args[0]) {
        print("Usage: exec <file>\n");
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
"""
commands["grep.c"] = r"""#include "../include/fs.h"
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
"""
commands["cp.c"] = r"""#include "cmd.h"

static int cp_strlen(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void cp_strcpy(char* dst, const char* src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

// Join base + "/" + name into out (avoids double slashes).
static void cp_join(char* out, const char* base, const char* name) {
    int len = cp_strlen(base);
    if (len == 0) { out[0] = '/'; len = 1; out[1] = 0; }
    else cp_strcpy(out, base, 256);
    if (out[len - 1] != '/') {
        if (len < 254) { out[len] = '/'; len++; out[len] = 0; }
    }
    int nlen = cp_strlen(name);
    for (int k = 0; k < nlen && len < 255; k++) out[len++] = name[k];
    out[len] = 0;
}

// basename(path): part after the last '/'
static const char* cp_basename(const char* path) {
    const char* base = path;
    const char* q = path;
    while (*q) {
        if (*q == '/' && *(q + 1)) base = q + 1;
        q++;
    }
    return base;
}

static int cp_copy_file(const char* src, const char* dst) {
    extern int fs_read_file(const char* path, char* buffer, uint32_t* size);
    extern int fs_write_file(const char* path, const char* data, uint32_t size);
    extern int fs_create_file(const char* path);
    extern int fs_file_exists(const char* path);

    char buffer[4096];
    uint32_t size = 0;

    if (fs_read_file(src, buffer, &size) != 0) return -1;
    if (!fs_file_exists(dst)) fs_create_file(dst);
    return fs_write_file(dst, buffer, size);
}

static int cp_copy_dir(const char* src, const char* dst) {
    extern int fs_create_directory(const char* path);
    extern int fs_directory_exists(const char* path);
    extern int fs_list_directory(const char* path, char* buffer, uint32_t* size);

    if (!fs_directory_exists(dst)) {
        if (fs_create_directory(dst) != 0) return -1;
    }

    char buf[4096];
    uint32_t size = 0;
    if (fs_list_directory(src, buf, &size) != 0) return -1;

    int i = 0;
    while (i < (int)size) {
        char entry[128];
        int j = 0;
        while (i < (int)size && buf[i] != '\n' && j < 127) { entry[j++] = buf[i++]; }
        entry[j] = 0;
        if (i < (int)size && buf[i] == '\n') i++;
        if (j == 0) continue;

        int is_dir = 0;
        if (entry[j - 1] == '/') { entry[j - 1] = 0; is_dir = 1; }

        char child_src[256];
        char child_dst[256];
        cp_join(child_src, src, entry);
        cp_join(child_dst, dst, entry);

        if (is_dir) {
            if (cp_copy_dir(child_src, child_dst) != 0) return -1;
        } else {
            if (cp_copy_file(child_src, child_dst) != 0) return -1;
        }
    }
    return 0;
}

void cmd_cp(char* args) {
    extern void print(const char* s);
    extern int fs_file_exists(const char* path);
    extern int fs_directory_exists(const char* path);

    if (!args || !*args) {
        print("Usage: cp <source> <destination>\n");
        return;
    }

    while (*args == ' ') args++;

    // Trim trailing whitespace/newline
    char* end = args;
    while (*end) end++;
    end--;
    while (end > args && (*end == ' ' || *end == '\n' || *end == '\r')) {
        *end = 0;
        end--;
    }

    // Split into source and destination on first space
    char* src = args;
    char* p = args;
    while (*p && *p != ' ') p++;

    if (!*p) {
        print("Usage: cp <source> <destination>\n");
        return;
    }

    *p = 0;
    p++;
    while (*p == ' ') p++;
    char* dst = p;

    if (!*dst) {
        print("Usage: cp <source> <destination>\n");
        return;
    }

    // Strip a single trailing slash from dst for existence checks
    char dst_trimmed[256];
    cp_strcpy(dst_trimmed, dst, 256);
    int dlen = cp_strlen(dst_trimmed);
    if (dlen > 1 && dst_trimmed[dlen - 1] == '/') dst_trimmed[dlen - 1] = 0;

    if (fs_directory_exists(src)) {
        char final_dst[256];
        if (fs_directory_exists(dst_trimmed)) {
            cp_join(final_dst, dst_trimmed, cp_basename(src));
        } else {
            cp_strcpy(final_dst, dst_trimmed, 256);
        }
        if (cp_copy_dir(src, final_dst) != 0) {
            print("cp: cannot copy directory '");
            print(src);
            print("'\n");
        }
        return;
    }

    if (!fs_file_exists(src)) {
        print("cp: cannot stat '");
        print(src);
        print("': No such file or directory\n");
        return;
    }

    char real_dst[256];
    if (fs_directory_exists(dst_trimmed)) {
        cp_join(real_dst, dst_trimmed, cp_basename(src));
    } else {
        cp_strcpy(real_dst, dst_trimmed, 256);
    }

    if (cp_copy_file(src, real_dst) != 0) {
        print("cp: cannot create regular file '");
        print(real_dst);
        print("'\n");
    }
}
"""
commands["mv.c"] = r"""#include "cmd.h"

static int mv_strlen(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void mv_strcpy(char* dst, const char* src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static void mv_join(char* out, const char* base, const char* name) {
    int len = mv_strlen(base);
    if (len == 0) { out[0] = '/'; len = 1; out[1] = 0; }
    else mv_strcpy(out, base, 256);
    if (out[len - 1] != '/') {
        if (len < 254) { out[len] = '/'; len++; out[len] = 0; }
    }
    int nlen = mv_strlen(name);
    for (int k = 0; k < nlen && len < 255; k++) out[len++] = name[k];
    out[len] = 0;
}

static const char* mv_basename(const char* path) {
    const char* base = path;
    const char* q = path;
    while (*q) {
        if (*q == '/' && *(q + 1)) base = q + 1;
        q++;
    }
    return base;
}

static int mv_copy_file(const char* src, const char* dst) {
    extern int fs_read_file(const char* path, char* buffer, uint32_t* size);
    extern int fs_write_file(const char* path, const char* data, uint32_t size);
    extern int fs_create_file(const char* path);
    extern int fs_file_exists(const char* path);

    char buffer[4096];
    uint32_t size = 0;

    if (fs_read_file(src, buffer, &size) != 0) return -1;
    if (!fs_file_exists(dst)) fs_create_file(dst);
    return fs_write_file(dst, buffer, size);
}

static int mv_copy_dir(const char* src, const char* dst) {
    extern int fs_create_directory(const char* path);
    extern int fs_directory_exists(const char* path);
    extern int fs_list_directory(const char* path, char* buffer, uint32_t* size);

    if (!fs_directory_exists(dst)) {
        if (fs_create_directory(dst) != 0) return -1;
    }

    char buf[4096];
    uint32_t size = 0;
    if (fs_list_directory(src, buf, &size) != 0) return -1;

    int i = 0;
    while (i < (int)size) {
        char entry[128];
        int j = 0;
        while (i < (int)size && buf[i] != '\n' && j < 127) { entry[j++] = buf[i++]; }
        entry[j] = 0;
        if (i < (int)size && buf[i] == '\n') i++;
        if (j == 0) continue;

        int is_dir = 0;
        if (entry[j - 1] == '/') { entry[j - 1] = 0; is_dir = 1; }

        char child_src[256];
        char child_dst[256];
        mv_join(child_src, src, entry);
        mv_join(child_dst, dst, entry);

        if (is_dir) {
            if (mv_copy_dir(child_src, child_dst) != 0) return -1;
        } else {
            if (mv_copy_file(child_src, child_dst) != 0) return -1;
        }
    }
    return 0;
}

// Deletes everything inside src (files + subdirs, depth-first), then src itself.
static int mv_remove_tree(const char* src) {
    extern int fs_delete_file(const char* path);
    extern int fs_delete_directory(const char* path);
    extern int fs_list_directory(const char* path, char* buffer, uint32_t* size);

    char buf[4096];
    uint32_t size = 0;
    if (fs_list_directory(src, buf, &size) != 0) return -1;

    int i = 0;
    while (i < (int)size) {
        char entry[128];
        int j = 0;
        while (i < (int)size && buf[i] != '\n' && j < 127) { entry[j++] = buf[i++]; }
        entry[j] = 0;
        if (i < (int)size && buf[i] == '\n') i++;
        if (j == 0) continue;

        int is_dir = 0;
        if (entry[j - 1] == '/') { entry[j - 1] = 0; is_dir = 1; }

        char child[256];
        mv_join(child, src, entry);

        if (is_dir) {
            if (mv_remove_tree(child) != 0) return -1;
        } else {
            fs_delete_file(child);
        }
    }

    return fs_delete_directory(src);
}

void cmd_mv(char* args) {
    extern void print(const char* s);
    extern int fs_file_exists(const char* path);
    extern int fs_directory_exists(const char* path);
    extern int fs_delete_file(const char* path);

    if (!args || !*args) {
        print("Usage: mv <source> <destination>\n");
        return;
    }

    while (*args == ' ') args++;

    // Trim trailing whitespace/newline
    char* end = args;
    while (*end) end++;
    end--;
    while (end > args && (*end == ' ' || *end == '\n' || *end == '\r')) {
        *end = 0;
        end--;
    }

    // Split into source and destination on first space
    char* src = args;
    char* p = args;
    while (*p && *p != ' ') p++;

    if (!*p) {
        print("Usage: mv <source> <destination>\n");
        return;
    }

    *p = 0;
    p++;
    while (*p == ' ') p++;
    char* dst = p;

    if (!*dst) {
        print("Usage: mv <source> <destination>\n");
        return;
    }

    // Strip a single trailing slash from dst for existence checks
    char dst_trimmed[256];
    mv_strcpy(dst_trimmed, dst, 256);
    int dlen = mv_strlen(dst_trimmed);
    if (dlen > 1 && dst_trimmed[dlen - 1] == '/') dst_trimmed[dlen - 1] = 0;

    if (fs_directory_exists(src)) {
        char final_dst[256];
        if (fs_directory_exists(dst_trimmed)) {
            mv_join(final_dst, dst_trimmed, mv_basename(src));
        } else {
            mv_strcpy(final_dst, dst_trimmed, 256);
        }
        if (mv_copy_dir(src, final_dst) != 0) {
            print("mv: cannot move directory '");
            print(src);
            print("'\n");
            return;
        }
        mv_remove_tree(src);
        return;
    }

    if (!fs_file_exists(src)) {
        print("mv: cannot stat '");
        print(src);
        print("': No such file or directory\n");
        return;
    }

    char real_dst[256];
    if (fs_directory_exists(dst_trimmed)) {
        mv_join(real_dst, dst_trimmed, mv_basename(src));
    } else {
        mv_strcpy(real_dst, dst_trimmed, 256);
    }

    if (mv_copy_file(src, real_dst) != 0) {
        print("mv: cannot create regular file '");
        print(real_dst);
        print("'\n");
        return;
    }

    fs_delete_file(src);
}
"""

os.makedirs("cmd", exist_ok=True)
for filename, content in commands.items():
    filepath = os.path.join("cmd", filename)
    if not os.path.exists(filepath):
        with open(filepath, "w") as f:
            f.write(content)
        print(f"[+] Created cmd/{filename}")
    else:
        print(f"[INFO] cmd/{filename} already exists")
print()
print("[INFO] Base commands ready")
print()
