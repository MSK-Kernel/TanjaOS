/* string.h - string and memory functions for TanjaOS (i386).
 *
 * The string/memory surface below is implemented in the kernel and
 * resolved by the ELF loader at exec time (kernel exports). */
#ifndef _STRING_H
#define _STRING_H

#include <stddef.h>
#include <stdint.h>

int    strlen(const char* s);
char*  strcpy(char* dst, const char* src);
char*  strncpy(char* dst, const char* src, unsigned int n);
char*  strcat(char* dst, const char* src);
int    strcmp(const char* a, const char* b);
int    strncmp(const char* a, const char* b, unsigned int n);
char*  strchr(const char* s, int c);
char*  strrchr(const char* s, int c);
char*  strstr(const char* haystack, const char* needle);
char*  strpbrk(const char* s, const char* accept);
int    strcasecmp(const char* a, const char* b);
int    strncasecmp(const char* a, const char* b, unsigned int n);
char*  strlwr(char* s);
char*  strupr(char* s);
char*  strerror(int errnum);

void*  memset(void* dst, int value, unsigned int count);
void*  memcpy(void* dst, const void* src, unsigned int count);
void*  memmove(void* dst, const void* src, unsigned int count);
int    memcmp(const void* a, const void* b, unsigned int count);

/* --- TanjaOS helpers --- */
int    streq(const char* a, const char* b);
void   clean(char* s);

#endif /* _STRING_H */
