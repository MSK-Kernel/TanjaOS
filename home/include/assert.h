/* assert.h */
#undef assert
#ifdef NDEBUG
#define assert(ignore) ((void)0)
#else
#include <stdio.h>
#include <stdlib.h>
#define assert(expr) ((expr) ? (void)0 : \
    (fprintf(stderr, "Assertion failed: %s, file %s, line %d\n", \
             #expr, __FILE__, __LINE__), exit(1)))
#endif
