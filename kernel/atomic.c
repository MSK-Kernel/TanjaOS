/* kernel/atomic.c - C11 stdatomic runtime helpers.
 *
 * tcc lowers atomics to calls like __atomic_store_4 / __atomic_load_8
 * (see tccgen.c parse_atomic), the same helpers libtcc1's stdatomic.c
 * and libatomic provide on hosted systems.  TanjaOS programs resolve
 * symbols against the kernel export table, so the kernel exports the
 * whole set (table entries in kernel/elf.c, prototypes in atomic.h).
 *
 * TanjaOS is single-CPU and programs run in ring 0, so cli/sti around
 * the access is a complete atomicity guarantee for every width,
 * including the 8-byte ops real i386 hardware cannot do in one
 * instruction.  The "memory" clobbers keep the compiler from moving
 * the access outside the cli..sti window.
 *
 * The memory-model parameter is accepted and ignored, matching
 * libatomic: on a single core with interrupts off, every ordering is
 * sequentially consistent.
 */
#include <stdint.h>
#include <stddef.h>

static inline void irq_off(void) { __asm__ __volatile__("cli" ::: "memory"); }
static inline void irq_on(void)  { __asm__ __volatile__("sti" ::: "memory"); }

/* ---- per-width load / store / exchange / compare_exchange ---- */

#define ATOMIC_BASIC_FUNCS(SZ, T) \
void __atomic_store_##SZ(volatile void* p, T v, int model) { \
    (void)model; irq_off(); *(volatile T*)p = v; irq_on(); \
} \
T __atomic_load_##SZ(volatile void* p, int model) { \
    T r; (void)model; irq_off(); r = *(volatile T*)p; irq_on(); return r; \
} \
T __atomic_exchange_##SZ(volatile void* p, T v, int model) { \
    T o; (void)model; irq_off(); o = *(volatile T*)p; *(volatile T*)p = v; irq_on(); return o; \
} \
int __atomic_compare_exchange_##SZ(volatile void* p, void* expected, \
                                   T desired, int weak, \
                                   int success, int failure) { \
    T cur; int ok; (void)weak; (void)success; (void)failure; \
    irq_off(); \
    cur = *(volatile T*)p; \
    if (cur == *(T*)expected) { *(volatile T*)p = desired; ok = 1; } \
    else { *(T*)expected = cur; ok = 0; } \
    irq_on(); \
    return ok; \
}

ATOMIC_BASIC_FUNCS(1, uint8_t)
ATOMIC_BASIC_FUNCS(2, uint16_t)
ATOMIC_BASIC_FUNCS(4, uint32_t)
ATOMIC_BASIC_FUNCS(8, uint64_t)

/* ---- read-modify-write: fetch_X returns old, X_fetch returns new ---- */

#define ATOMIC_RMW_FUNCS(SZ, T, NAME, NEWV) \
T __atomic_fetch_##NAME##_##SZ(volatile void* p, T v, int model) { \
    T o, n; (void)model; irq_off(); o = *(volatile T*)p; n = (T)(NEWV); \
    *(volatile T*)p = n; irq_on(); return o; \
} \
T __atomic_##NAME##_fetch_##SZ(volatile void* p, T v, int model) { \
    T o, n; (void)model; irq_off(); o = *(volatile T*)p; n = (T)(NEWV); \
    *(volatile T*)p = n; irq_on(); return n; \
}

#define ATOMIC_ALL_RMW(SZ, T) \
    ATOMIC_RMW_FUNCS(SZ, T, add, o + v) \
    ATOMIC_RMW_FUNCS(SZ, T, sub, o - v) \
    ATOMIC_RMW_FUNCS(SZ, T, and, o & v) \
    ATOMIC_RMW_FUNCS(SZ, T, or,  o | v) \
    ATOMIC_RMW_FUNCS(SZ, T, xor, o ^ v) \
    ATOMIC_RMW_FUNCS(SZ, T, nand, ~(o & v))

ATOMIC_ALL_RMW(1, uint8_t)
ATOMIC_ALL_RMW(2, uint16_t)
ATOMIC_ALL_RMW(4, uint32_t)
ATOMIC_ALL_RMW(8, uint64_t)

/* ---- the helpers <stdatomic.h> itself declares ---- */

void atomic_thread_fence(int model)  { (void)model; irq_off(); irq_on(); }
void atomic_signal_fence(int model)  { (void)model; }

/* atomic_flag is struct { _Atomic _Bool value; }: set to 1, return old. */
int atomic_flag_test_and_set(void* object) {
    volatile unsigned char* f = (volatile unsigned char*)object;
    int old;
    irq_off(); old = *f; *f = 1; irq_on();
    return old ? 1 : 0;
}

int atomic_flag_test_and_set_explicit(void* object, int model) {
    (void)model;
    return atomic_flag_test_and_set(object);
}

void atomic_flag_clear(void* object) {
    volatile unsigned char* f = (volatile unsigned char*)object;
    irq_off(); *f = 0; irq_on();
}

void atomic_flag_clear_explicit(void* object, int model) {
    (void)model;
    atomic_flag_clear(object);
}

/* All supported widths (1/2/4/8, see parse_atomic's size check) are
 * always lock-free here. */
int __atomic_is_lock_free(size_t size, void* ptr) {
    (void)ptr;
    return size == 1 || size == 2 || size == 4 || size == 8;
}

/* Include the X-macro prototype list last: if any definition above
 * drifts from what kernel/elf.c exports, this file fails to build. */
#include "atomic.h"
#define TANJA_ATOMIC_PROTO_CHECK(name, ret, params) \
    extern ret name params;
TANJA_ATOMIC_ALL(TANJA_ATOMIC_PROTO_CHECK)
#undef TANJA_ATOMIC_PROTO_CHECK
