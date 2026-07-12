#!/usr/bin/env python3
import os

commands = {}

commands["help.c"] = """#include "cmd.h"

void cmd_help(char* args) {
    (void)args;
    extern void list_commands(void);
    list_commands();
}
"""

commands["reboot.c"] = """#include "cmd.h"

void cmd_reboot(char* args) {
    (void)args;
    extern void print(const char* s);
    extern uint8_t inb(uint16_t port);
    extern void outb(uint16_t port, uint8_t val);
    print("Rebooting...\\n");
    for (volatile int i = 0; i < 1000000; i++);
    while ((inb(0x64) & 0x02) != 0);
    outb(0x64, 0xFE);
    asm volatile ("lidt 0\\n" "int $0");
    while (1);
}
"""

commands["echo.c"] = """#include "cmd.h"

void cmd_echo(char* args) {
    extern void print(const char* s);
    extern void putc(char c);
    extern int fs_write_file(const char* path, const char* data, uint32_t size);
    extern int fs_file_exists(const char* path);
    
    if (!args || !*args) {
        putc('\\n');
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
                while (end > filename && (*end == ' ' || *end == '\\n' || *end == '\\r')) {
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
    } else if (*text == '\\'') {
        text++;
        end = text;
        while (*end && *end != '\\'') end++;
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
        putc('\\n');
    }
}
"""

commands["clear.c"] = """#include "cmd.h"

void cmd_clear(char* args) {
    (void)args;
    extern void clear_screen(void);
    clear_screen();
}
"""

commands["mkdir.c"] = """#include "cmd.h"

void cmd_mkdir(char* args) {
    extern void print(const char* s);
    extern int fs_create_directory(const char* path);
    extern int fs_directory_exists(const char* path);
    
    if (!args || !*args) {
        print("Usage: mkdir <directory>\\n");
        return;
    }
    
    while (*args == ' ') args++;
    
    char* end = args;
    while (*end) end++;
    end--;
    while (end > args && (*end == ' ' || *end == '\\n' || *end == '\\r')) {
        *end = 0;
        end--;
    }
    
    if (fs_directory_exists(args)) {
        print("mkdir: cannot create directory '");
        print(args);
        print("': File exists\\n");
        return;
    }
    
    if (fs_create_directory(args) != 0) {
        print("mkdir: cannot create directory '");
        print(args);
        print("': Error\\n");
    }
}
"""

commands["rmdir.c"] = """#include "cmd.h"

void cmd_rmdir(char* args) {
    extern void print(const char* s);
    extern int fs_delete_directory(const char* path);
    extern int fs_directory_exists(const char* path);
    
    if (!args || !*args) {
        print("Usage: rmdir <directory>\\n");
        return;
    }
    
    while (*args == ' ') args++;
    
    char* end = args;
    while (*end) end++;
    end--;
    while (end > args && (*end == ' ' || *end == '\\n' || *end == '\\r')) {
        *end = 0;
        end--;
    }
    
    if (!fs_directory_exists(args)) {
        print("rmdir: failed to remove '");
        print(args);
        print("': No such file or directory\\n");
        return;
    }
    
    int result = fs_delete_directory(args);
    if (result == -2) {
        print("rmdir: failed to remove '");
        print(args);
        print("': Directory not empty\\n");
    } else if (result != 0) {
        print("rmdir: failed to remove '");
        print(args);
        print("': Error\\n");
    }
}
"""

commands["touch.c"] = """#include "cmd.h"

void cmd_touch(char* args) {
    extern void print(const char* s);
    extern int fs_create_file(const char* path);
    extern int fs_file_exists(const char* path);
    
    if (!args || !*args) {
        print("Usage: touch <file>\\n");
        return;
    }
    
    while (*args == ' ') args++;
    
    char* end = args;
    while (*end) end++;
    end--;
    while (end > args && (*end == ' ' || *end == '\\n' || *end == '\\r')) {
        *end = 0;
        end--;
    }
    
    if (!fs_file_exists(args)) {
        if (fs_create_file(args) != 0) {
            print("touch: cannot touch '");
            print(args);
            print("': Error\\n");
        }
    }
}
"""

commands["rm.c"] = """#include "cmd.h"

void cmd_rm(char* args) {
    extern void print(const char* s);
    extern int fs_delete_file(const char* path);
    extern int fs_file_exists(const char* path);
    
    if (!args || !*args) {
        print("Usage: rm <file>\\n");
        return;
    }
    
    while (*args == ' ') args++;
    
    char* end = args;
    while (*end) end++;
    end--;
    while (end > args && (*end == ' ' || *end == '\\n' || *end == '\\r')) {
        *end = 0;
        end--;
    }
    
    if (!fs_file_exists(args)) {
        print("rm: cannot remove '");
        print(args);
        print("': No such file or directory\\n");
        return;
    }
    
    fs_delete_file(args);
}
"""

commands["cat.c"] = """#include "cmd.h"

void cmd_cat(char* args) {
    extern void print(const char* s);
    extern void putc(char c);
    extern int fs_read_file(const char* path, char* buffer, uint32_t* size);
    extern int fs_file_exists(const char* path);
    
    if (!args || !*args) {
        print("Usage: cat <file>\\n");
        return;
    }
    
    while (*args == ' ') args++;
    
    char* end = args;
    while (*end) end++;
    end--;
    while (end > args && (*end == ' ' || *end == '\\n' || *end == '\\r')) {
        *end = 0;
        end--;
    }
    
    if (!fs_file_exists(args)) {
        print("cat: ");
        print(args);
        print(": No such file or directory\\n");
        return;
    }
    
    char buffer[4096];
    uint32_t size = 0;
    
    if (fs_read_file(args, buffer, &size) == 0) {
        for (uint32_t i = 0; i < size; i++) {
            putc(buffer[i]);
        }
        if (size > 0 && buffer[size-1] != '\\n') {
            putc('\\n');
        }
    }
}
"""

commands["ls.c"] = """#include "cmd.h"

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

commands["pwd.c"] = """#include "cmd.h"

void cmd_pwd(char* args) {
    (void)args;
    extern void print(const char* s);
    extern void fs_get_current_path(char* path);
    
    char path[256];
    fs_get_current_path(path);
    print(path);
    print("\\n");
}
"""

commands["cd.c"] = """#include "cmd.h"

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
    while (end > args && (*end == ' ' || *end == '\\n' || *end == '\\r')) {
        *end = 0;
        end--;
    }
    
    if (fs_change_directory(args) != 0) {
        print("cd: ");
        print(args);
        print(": No such file or directory\\n");
    }
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
