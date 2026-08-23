#ifndef CC_H
#define CC_H

// Compiles the given C source in-memory and, if compilation succeeds,
// calls its main() immediately. Errors are printed to the console with
// a line number and nothing is executed.
void cc_compile_and_run(const char* source, unsigned int len);

#endif
