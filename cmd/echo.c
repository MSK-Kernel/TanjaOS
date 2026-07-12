#include "cmd.h"

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
