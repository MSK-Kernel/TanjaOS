#ifndef T_STDLIB_H
#define T_STDLIB_H
#include <stddef.h>
void* malloc(size_t);
void* calloc(size_t, size_t);
void* realloc(void*, size_t);
void free(void*);
void exit(int) __attribute__((noreturn));
void abort(void) __attribute__((noreturn));
int atexit(void (*)(void));
int atoi(const char*);
double atof(const char*);
long strtol(const char*, char**, int);
unsigned long strtoul(const char*, char**, int);
int abs(int);
int rand(void);
void srand(unsigned int);
char* getenv(const char*);
int system(const char*);
void qsort(void*, size_t, size_t, int (*)(const void*, const void*));
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 32767
#endif
