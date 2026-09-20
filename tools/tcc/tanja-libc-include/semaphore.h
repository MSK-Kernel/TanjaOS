#ifndef WRAPPER_SEMAPHORE_H
#define WRAPPER_SEMAPHORE_H
/* TanjaOS is single-threaded: a counting int is a perfectly good semaphore. */
#include "../tanja-libc.h"
typedef int sem_t;
static inline int sem_init(sem_t *s, int pshared, unsigned v)
{ (void)pshared; *s = (int)v; return 0; }
static inline int sem_wait(sem_t *s) { if (*s > 0) --*s; return 0; }
static inline int sem_post(sem_t *s) { ++*s; return 0; }
static inline int sem_destroy(sem_t *s) { (void)s; return 0; }
#endif
