#ifndef CC_H
#define CC_H

#include <stdint.h>

/* Compile C source into the TanjaOS x86-32 executable format. */
int cc_compile_to_binary(const char* source, unsigned int len,
                         uint8_t* out, unsigned int out_cap,
                         unsigned int* out_len);

/* Load and execute a previously compiled TanjaOS binary. */
int cc_execute_binary(const uint8_t* image, unsigned int len);

/* Kept for compatibility with kernel-side tests/tools. */
void cc_compile_and_run(const char* source, unsigned int len);
int cc_compile_only(const char* source, unsigned int len);
void* cc_debug_codebuf(void);
unsigned int cc_debug_codebuf_size(void);
unsigned int cc_debug_codelen(void);

#endif
