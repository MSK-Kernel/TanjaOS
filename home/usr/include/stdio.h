/* stdio.h - Standard I/O for TanjaOS (i386).
 *
 * The full C stdio surface (printf/scanf/fopen/...) is implemented
 * by the kernel C library (kernel/libc.c) and resolved by the ELF
 * loader at exec time. The TanjaOS kernel console exports are
 * marked below. */
#ifndef _STDIO_H
#define _STDIO_H

#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct {
    int fd;
    int eof;
    int error;
    int unget_buf;
} FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

#define EOF (-1)

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* file stdio (tcc libc shim) */
FILE *fopen(const char *path, const char *mode);
int fclose(FILE *fp);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *fp);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *fp);
int fseek(FILE *fp, long offset, int whence);
long ftell(FILE *fp);
int feof(FILE *fp);
int ferror(FILE *fp);
int fflush(FILE *fp);
int fgetc(FILE *fp);
int getc(FILE *fp);
int getchar(void);
int ungetc(int c, FILE *fp);
char *fgets(char *s, int size, FILE *fp);
int fputs(const char *s, FILE *fp);
int fputc(int c, FILE *fp);
int putc(int c, FILE *fp);
FILE *fdopen(int fd, const char *mode);
FILE *freopen(const char *path, const char *mode, FILE *stream);

/* file operations */
int remove(const char *path);
int rename(const char *oldpath, const char *newpath);

/* formatted output */
int vsnprintf(char *str, size_t size, const char *format, va_list ap);
int snprintf(char *str, size_t size, const char *format, ...);
int sprintf(char *str, const char *format, ...);
int vfprintf(FILE *stream, const char *format, va_list ap);
int fprintf(FILE *stream, const char *format, ...);
int vprintf(const char *format, va_list ap);
int printf(const char *format, ...);

int puts(const char *s);
int putchar(int c);

/* TanjaOS kernel console exports */
void print(const char* s);
void print_n(const char* s, uint32_t len);
void print_color(const char* s, uint16_t color);
void putc_color(char c, uint16_t color);
void clear_screen(void);
void print_dec(uint32_t n);
void print_dec_pad(uint32_t n, int width);
void print_hex(uint32_t n);

/* Direct VGA text access for full-screen programs. cursor is the
 * current cell index into VGA[]. */
extern uint16_t* VGA;
extern int cursor;
void sync_cursor(void);

/* formatted input */
int scanf(const char *format, ...);
int sscanf(const char *str, const char *format, ...);
int fscanf(FILE *stream, const char *format, ...);
int vscanf(const char *format, va_list ap);
int vsscanf(const char *str, const char *format, va_list ap);
int vfscanf(FILE *stream, const char *format, va_list ap);

/* TanjaOS keyboard */
int get_key(void);            /* blocks; returns ASCII or a KEY_* code */
int key_available(void);
/* extras */
void perror(const char *s);
void rewind(FILE *f);
void clearerr(FILE *f);
int setvbuf(FILE *f, char *buf, int mode, size_t size);
void setbuf(FILE *f, char *buf);
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2

void read_line(char* buf, int max_len);

#define KEY_UP        0x80
#define KEY_DOWN      0x81
#define KEY_LEFT      0x82
#define KEY_RIGHT     0x83
#define KEY_ENTER     0x84
#define KEY_BACKSPACE 0x85

/* VGA colors: attribute already shifted, black background. */
#define COLOR_WHITE       (0x0F << 8)
#define COLOR_LIGHT_GREEN (0x0A << 8)
#define COLOR_DIR         (0x09 << 8)

#endif /* _STDIO_H */
