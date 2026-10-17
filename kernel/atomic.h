/* kernel/atomic.h - stdatomic runtime helpers exported to programs.
 *
 * tcc lowers C11 atomics (_Atomic types, __atomic builtins used by
 * /include/stdatomic.h) to calls to __atomic_<op>_<size> helpers, the
 * same set libtcc1/libatomic provide on a hosted system.  TanjaOS has
 * no linker, so the kernel itself implements and exports them.
 *
 * Kept as an X-macro list - each entry is X(name, ret, params) - so
 * kernel/elf.c can extern and export every helper without
 * hand-syncing 60+ prototypes; kernel/atomic.c includes this after
 * defining them, so any drift is a compile error.
 */
#ifndef TANJA_ATOMIC_H
#define TANJA_ATOMIC_H
#include <stdint.h>
#include <stddef.h>

#define TANJA_ATOMIC_LOADS(X) \
    X(__atomic_load_1,  uint8_t,  (volatile void*, int)) \
    X(__atomic_load_2,  uint16_t, (volatile void*, int)) \
    X(__atomic_load_4,  uint32_t, (volatile void*, int)) \
    X(__atomic_load_8,  uint64_t, (volatile void*, int))

#define TANJA_ATOMIC_STORES(X) \
    X(__atomic_store_1, void, (volatile void*, uint8_t,  int)) \
    X(__atomic_store_2, void, (volatile void*, uint16_t, int)) \
    X(__atomic_store_4, void, (volatile void*, uint32_t, int)) \
    X(__atomic_store_8, void, (volatile void*, uint64_t, int))

#define TANJA_ATOMIC_EXCHANGES(X) \
    X(__atomic_exchange_1, uint8_t,  (volatile void*, uint8_t,  int)) \
    X(__atomic_exchange_2, uint16_t, (volatile void*, uint16_t, int)) \
    X(__atomic_exchange_4, uint32_t, (volatile void*, uint32_t, int)) \
    X(__atomic_exchange_8, uint64_t, (volatile void*, uint64_t, int))

#define TANJA_ATOMIC_CMPXCHGS(X) \
    X(__atomic_compare_exchange_1, int, (volatile void*, void*, uint8_t,  int, int, int)) \
    X(__atomic_compare_exchange_2, int, (volatile void*, void*, uint16_t, int, int, int)) \
    X(__atomic_compare_exchange_4, int, (volatile void*, void*, uint32_t, int, int, int)) \
    X(__atomic_compare_exchange_8, int, (volatile void*, void*, uint64_t, int, int, int))

/* fetch_X returns the OLD value, X_fetch returns the NEW value */
#define TANJA_ATOMIC_RMWS(X) \
    X(__atomic_fetch_add_1, uint8_t,  (volatile void*, uint8_t,  int)) \
    X(__atomic_fetch_add_2, uint16_t, (volatile void*, uint16_t, int)) \
    X(__atomic_fetch_add_4, uint32_t, (volatile void*, uint32_t, int)) \
    X(__atomic_fetch_add_8, uint64_t, (volatile void*, uint64_t, int)) \
    X(__atomic_add_fetch_1, uint8_t,  (volatile void*, uint8_t,  int)) \
    X(__atomic_add_fetch_2, uint16_t, (volatile void*, uint16_t, int)) \
    X(__atomic_add_fetch_4, uint32_t, (volatile void*, uint32_t, int)) \
    X(__atomic_add_fetch_8, uint64_t, (volatile void*, uint64_t, int)) \
    X(__atomic_fetch_sub_1, uint8_t,  (volatile void*, uint8_t,  int)) \
    X(__atomic_fetch_sub_2, uint16_t, (volatile void*, uint16_t, int)) \
    X(__atomic_fetch_sub_4, uint32_t, (volatile void*, uint32_t, int)) \
    X(__atomic_fetch_sub_8, uint64_t, (volatile void*, uint64_t, int)) \
    X(__atomic_sub_fetch_1, uint8_t,  (volatile void*, uint8_t,  int)) \
    X(__atomic_sub_fetch_2, uint16_t, (volatile void*, uint16_t, int)) \
    X(__atomic_sub_fetch_4, uint32_t, (volatile void*, uint32_t, int)) \
    X(__atomic_sub_fetch_8, uint64_t, (volatile void*, uint64_t, int)) \
    X(__atomic_fetch_and_1, uint8_t,  (volatile void*, uint8_t,  int)) \
    X(__atomic_fetch_and_2, uint16_t, (volatile void*, uint16_t, int)) \
    X(__atomic_fetch_and_4, uint32_t, (volatile void*, uint32_t, int)) \
    X(__atomic_fetch_and_8, uint64_t, (volatile void*, uint64_t, int)) \
    X(__atomic_and_fetch_1, uint8_t,  (volatile void*, uint8_t,  int)) \
    X(__atomic_and_fetch_2, uint16_t, (volatile void*, uint16_t, int)) \
    X(__atomic_and_fetch_4, uint32_t, (volatile void*, uint32_t, int)) \
    X(__atomic_and_fetch_8, uint64_t, (volatile void*, uint64_t, int)) \
    X(__atomic_fetch_or_1, uint8_t,  (volatile void*, uint8_t,  int)) \
    X(__atomic_fetch_or_2, uint16_t, (volatile void*, uint16_t, int)) \
    X(__atomic_fetch_or_4, uint32_t, (volatile void*, uint32_t, int)) \
    X(__atomic_fetch_or_8, uint64_t, (volatile void*, uint64_t, int)) \
    X(__atomic_or_fetch_1, uint8_t,  (volatile void*, uint8_t,  int)) \
    X(__atomic_or_fetch_2, uint16_t, (volatile void*, uint16_t, int)) \
    X(__atomic_or_fetch_4, uint32_t, (volatile void*, uint32_t, int)) \
    X(__atomic_or_fetch_8, uint64_t, (volatile void*, uint64_t, int)) \
    X(__atomic_fetch_xor_1, uint8_t,  (volatile void*, uint8_t,  int)) \
    X(__atomic_fetch_xor_2, uint16_t, (volatile void*, uint16_t, int)) \
    X(__atomic_fetch_xor_4, uint32_t, (volatile void*, uint32_t, int)) \
    X(__atomic_fetch_xor_8, uint64_t, (volatile void*, uint64_t, int)) \
    X(__atomic_xor_fetch_1, uint8_t,  (volatile void*, uint8_t,  int)) \
    X(__atomic_xor_fetch_2, uint16_t, (volatile void*, uint16_t, int)) \
    X(__atomic_xor_fetch_4, uint32_t, (volatile void*, uint32_t, int)) \
    X(__atomic_xor_fetch_8, uint64_t, (volatile void*, uint64_t, int)) \
    X(__atomic_fetch_nand_1, uint8_t,  (volatile void*, uint8_t,  int)) \
    X(__atomic_fetch_nand_2, uint16_t, (volatile void*, uint16_t, int)) \
    X(__atomic_fetch_nand_4, uint32_t, (volatile void*, uint32_t, int)) \
    X(__atomic_fetch_nand_8, uint64_t, (volatile void*, uint64_t, int)) \
    X(__atomic_nand_fetch_1, uint8_t,  (volatile void*, uint8_t,  int)) \
    X(__atomic_nand_fetch_2, uint16_t, (volatile void*, uint16_t, int)) \
    X(__atomic_nand_fetch_4, uint32_t, (volatile void*, uint32_t, int)) \
    X(__atomic_nand_fetch_8, uint64_t, (volatile void*, uint64_t, int))

/* The non-suffixed functions stdatomic.h itself declares (fences,
 * atomic_flag, is_lock_free).  _Bool returns are declared int: only
 * the address is taken, and the helpers return 0/1 in eax so al is
 * valid either way. */
#define TANJA_ATOMIC_MISC(X) \
    X(atomic_thread_fence,                void, (int)) \
    X(atomic_signal_fence,               void, (int)) \
    X(atomic_flag_test_and_set,          int,  (void*)) \
    X(atomic_flag_test_and_set_explicit, int,  (void*, int)) \
    X(atomic_flag_clear,                 void, (void*)) \
    X(atomic_flag_clear_explicit,        void, (void*, int)) \
    X(__atomic_is_lock_free,             int,  (size_t, void*))

#define TANJA_ATOMIC_ALL(X) \
    TANJA_ATOMIC_LOADS(X) \
    TANJA_ATOMIC_STORES(X) \
    TANJA_ATOMIC_EXCHANGES(X) \
    TANJA_ATOMIC_CMPXCHGS(X) \
    TANJA_ATOMIC_RMWS(X) \
    TANJA_ATOMIC_MISC(X)

#endif /* TANJA_ATOMIC_H */
