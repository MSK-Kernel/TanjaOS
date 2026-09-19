#ifndef WRAPPER_DLFCN_H
#define WRAPPER_DLFCN_H
/* TanjaOS has no dynamic linker. tcc uses dlsym only in -run mode. */
#include "../tanja-libc.h"
#define RTLD_LAZY 1
#define RTLD_NOW 2
#define RTLD_GLOBAL 0x100
#define RTLD_LOCAL 0
#define RTLD_DEFAULT ((void*)0)
void *dlopen(const char *filename, int flag);
void *dlsym(void *handle, const char *symbol);
int dlclose(void *handle);
char *dlerror(void);
#endif
