#ifndef WRAPPER_SYS_MMAN_H
#define WRAPPER_SYS_MMAN_H
/* TanjaOS has no mmap. tcc -run would need it; compile mode (-c) never does.
 * Stubs return failure so any stray call fails loudly, not silently. */
#include "../tanja-libc.h"
#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 0x20
void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int mprotect(void *addr, size_t len, int prot);
int munmap(void *addr, size_t length);
#endif
