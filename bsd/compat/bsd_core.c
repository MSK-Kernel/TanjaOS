/* bsd_core.c - the OpenBSD kernel services the Wi-Fi code expects, built on top of
 * TanjaOS. Single-threaded and cooperative: "interrupts", timeouts and tasks are all
 * run from bsd_poll(), which the idle loop calls; tsleep() just spins calling it.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/timeout.h>
#include <sys/task.h>
#include <sys/refcnt.h>
#include <sys/rwlock.h>
#include <sys/time.h>
#include <sys/errno.h>
#include <sys/mbuf.h>
#include <sys/mutex.h>

/* TanjaOS kernel functions (their real names, bypassing the bsd_ renames) */
extern unsigned int get_uptime_ms(void);
extern void  timer_delay_ms(unsigned int ms);
extern int   fs_file_exists(const char* path);
extern unsigned int fs_get_file_size(const char* path);
extern int   fs_read_file(const char* path, char* buf, unsigned int* size);
extern int   printf(const char* fmt, ...);

static inline unsigned char io_inb(unsigned short p) { unsigned char v; __asm volatile("inb %1,%0" : "=a"(v) : "Nd"(p)); return v; }

/* ------------------------------------------------------------- heap */

#define HEAP_SZ (8u << 20)
static unsigned char heap[HEAP_SZ] __attribute__((aligned(4096)));
static unsigned int heap_top;
static void* free_list[28];

struct bh { unsigned int cls, pad[3]; };

void *malloc(size_t sz, int type, int flags) {
    unsigned int cls = 5, need = (unsigned int)sz + sizeof(struct bh);
    struct bh* h;
    while ((1u << cls) < need) cls++;
    if (cls >= 28) return NULL;
    if (free_list[cls]) {
        h = free_list[cls];
        free_list[cls] = *(void**)(h + 1);
    } else {
        unsigned int sz2 = 1u << cls;
        if (heap_top + sz2 > HEAP_SZ) { printf("bsd: out of memory (%u)\n", (unsigned)sz); return NULL; }
        h = (struct bh*)(heap + heap_top);
        heap_top += sz2;
        h->cls = cls;
    }
    if (flags & M_ZERO) memset(h + 1, 0, (1u << cls) - sizeof(struct bh));
    return h + 1;
}

void *mallocarray(size_t nmemb, size_t size, int type, int flags) {
    if (size && nmemb > 0xffffffffu / size) return NULL;
    return malloc(nmemb * size, type, flags);
}

void free(void *p, int type, size_t sz) {
    struct bh* h;
    if (!p) return;
    h = (struct bh*)p - 1;
    *(void**)(h + 1) = free_list[h->cls];
    free_list[h->cls] = h;
}

/* ------------------------------------------------------ small helpers */

void panic(const char *fmt, ...) {
    char b[200];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    __builtin_va_end(ap);
    printf("\nbsd panic: %s\n", b);
    for (;;) __asm volatile("cli; hlt");
}

void log(int level, const char *fmt, ...) {
    char b[200];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    __builtin_va_end(ap);
    printf("%s", b);
}

void explicit_bzero(void *p, size_t n) { volatile unsigned char* v = p; while (n--) *v++ = 0; }

size_t strlcpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    if (n) { for (; i + 1 < n && src[i]; i++) dst[i] = src[i]; dst[i] = 0; }
    while (src[i]) i++;
    return i;
}

int timingsafe_bcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b; unsigned char d = 0;
    while (n--) d |= *x++ ^ *y++;
    return d != 0;
}

int suser(struct proc *p) { return 0; }
int copyin(const void *u, void *k, size_t n) { memcpy(k, u, n); return 0; }
int copyout(const void *k, void *u, size_t n) { memcpy(u, k, n); return 0; }

/* ------------------------------------------------------------- time */

time_t getuptime(void) { return get_uptime_ms() / 1000; }

void getmicrouptime(struct timeval *tv) {
    unsigned int ms = get_uptime_ms();
    tv->tv_sec = ms / 1000;
    tv->tv_usec = (ms % 1000) * 1000;
}

static void delay_impl(int us) {
    if (us >= 2000) { timer_delay_ms(us / 1000); us %= 1000; }
    while (us-- > 0) io_inb(0x80);                 /* ~1 us per port 0x80 read */
}
void (*delay_func)(int) = delay_impl;

int ratecheck(struct timeval *last, const struct timeval *mininterval) {
    struct timeval now, d;
    getmicrouptime(&now);
    d.tv_sec = now.tv_sec - last->tv_sec; d.tv_usec = now.tv_usec - last->tv_usec;
    if (d.tv_usec < 0) { d.tv_sec--; d.tv_usec += 1000000; }
    if (last->tv_sec == 0 && last->tv_usec == 0) { *last = now; return 1; }
    if (d.tv_sec > mininterval->tv_sec || (d.tv_sec == mininterval->tv_sec && d.tv_usec >= mininterval->tv_usec)) {
        *last = now; return 1;
    }
    return 0;
}

/* ----------------------------------------------------------- random */

static unsigned int rs[4] = { 0x9e3779b9, 0x243f6a88, 0xb7e15162, 0x93c467e3 };
static int rs_seeded;

static unsigned int rnd_next(void) {
    unsigned int t;
    if (!rs_seeded) {
        unsigned int lo, hi;
        __asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
        rs[0] ^= lo; rs[1] ^= hi; rs[2] ^= get_uptime_ms() * 2654435761u;
        rs_seeded = 1;
    }
    t = rs[0] ^ (rs[0] << 11);
    rs[0] = rs[1]; rs[1] = rs[2]; rs[2] = rs[3];
    rs[3] = rs[3] ^ (rs[3] >> 19) ^ t ^ (t >> 8);
    { unsigned int lo; __asm volatile("rdtsc" : "=a"(lo)); rs[3] ^= lo & 0xff; }
    return rs[3];
}
uint32_t arc4random(void) { return rnd_next(); }
void arc4random_buf(void *buf, size_t n) {
    unsigned char* p = buf;
    while (n) { unsigned int r = rnd_next(); size_t k = n < 4 ? n : 4; memcpy(p, &r, k); p += k; n -= k; }
}
uint32_t arc4random_uniform(uint32_t upper) {
    uint32_t min, r;
    if (upper < 2) return 0;
    min = -upper % upper;
    do r = rnd_next(); while (r < min);
    return r % upper;
}

/* ---------------------------------------------------- locks / refcnt */

void mtx_enter(struct mutex *mtx) { }
void mtx_leave(struct mutex *mtx) { }
int splraise(int ipl) { return 0; }
void splx(int s) { }
void _rw_init_flags(struct rwlock *rwl, const char *name, int flags, const struct lock_type *type, int trace) { }
int rw_enter(struct rwlock *rwl, int flags) { return 0; }
void rw_enter_write(struct rwlock *rwl) { }
void rw_exit(struct rwlock *rwl) { }

void refcnt_init(struct refcnt *r) { r->r_refs = 1; }
void refcnt_take(struct refcnt *r) { r->r_refs++; }
int refcnt_rele(struct refcnt *r) { return --r->r_refs == 0; }
void refcnt_rele_wake(struct refcnt *r) { r->r_refs--; }
void refcnt_finalize(struct refcnt *r, const char *wmesg) { }

/* -------------------------------------------- timeouts, tasks, sleep */

#define MAX_TO 128
static struct timeout* to_tab[MAX_TO];
#define MAX_TASK 64
static struct task* task_q[MAX_TASK];
static int task_n;
struct taskq { int dummy; };
static struct taskq systq_obj;
struct taskq *const systq = &systq_obj;

struct taskq *taskq_create(const char *name, unsigned int nthreads, int ipl, unsigned int flags) { return &systq_obj; }

void timeout_set(struct timeout *to, void (*fn)(void *), void *arg) {
    to->to_func = fn; to->to_arg = arg; to->to_flags = TIMEOUT_INITIALIZED; to->to_time = 0;
}

static int to_add_ms(struct timeout *to, unsigned int ms) {
    int i, was = (to->to_flags & TIMEOUT_ONQUEUE) != 0, freeslot = -1;
    to->to_time = (int)(get_uptime_ms() + ms);
    if (was) return 0;
    for (i = 0; i < MAX_TO; i++) if (!to_tab[i]) { freeslot = i; break; }
    if (freeslot < 0) { printf("bsd: too many timeouts\n"); return 0; }
    to_tab[freeslot] = to;
    to->to_flags |= TIMEOUT_ONQUEUE;
    to->to_flags &= ~TIMEOUT_TRIGGERED;
    return 1;
}
int timeout_add_msec(struct timeout *to, uint64_t ms) { return to_add_ms(to, (unsigned int)ms); }
int timeout_add_sec(struct timeout *to, int s)   { return to_add_ms(to, s * 1000); }
int timeout_add_usec(struct timeout *to, uint64_t us) { return to_add_ms(to, (unsigned int)((us + 999) / 1000)); }
int timeout_del(struct timeout *to) {
    int i, was = (to->to_flags & TIMEOUT_ONQUEUE) != 0;
    if (was) for (i = 0; i < MAX_TO; i++) if (to_tab[i] == to) to_tab[i] = NULL;
    to->to_flags &= ~TIMEOUT_ONQUEUE;
    return was;
}

void task_set(struct task *t, void (*fn)(void *), void *arg) { t->t_func = fn; t->t_arg = arg; t->t_flags = 0; }
int task_add(struct taskq *tq, struct task *t) {
    if (t->t_flags & TASK_ONQUEUE) return 0;
    if (task_n >= MAX_TASK) { printf("bsd: task queue full\n"); return 0; }
    task_q[task_n++] = t;
    t->t_flags |= TASK_ONQUEUE;
    return 1;
}
int task_del(struct taskq *tq, struct task *t) {
    int i, j;
    if (!(t->t_flags & TASK_ONQUEUE)) return 0;
    for (i = 0; i < task_n; i++) if (task_q[i] == t) { for (j = i; j + 1 < task_n; j++) task_q[j] = task_q[j + 1]; task_n--; break; }
    t->t_flags &= ~TASK_ONQUEUE;
    return 1;
}

/* interrupt handlers registered by bsd_pci.c */
extern void bsd_run_irq_handlers(void);

static int poll_depth;

void bsd_poll(void) {
    int i, n;
    if (poll_depth >= 4) return;
    poll_depth++;
    bsd_run_irq_handlers();
    for (i = 0; i < MAX_TO; i++) {                        /* due timeouts */
        struct timeout* to = to_tab[i];
        if (to && (int)(get_uptime_ms() - (unsigned int)to->to_time) >= 0) {
            to_tab[i] = NULL;
            to->to_flags &= ~TIMEOUT_ONQUEUE;
            to->to_flags |= TIMEOUT_TRIGGERED;
            to->to_func(to->to_arg);
        }
    }
    for (n = 0; task_n && n < 32; n++) {                  /* pending tasks, in order */
        struct task* t = task_q[0];
        int j;
        for (j = 0; j + 1 < task_n; j++) task_q[j] = task_q[j + 1];
        task_n--;
        t->t_flags &= ~TASK_ONQUEUE;
        t->t_func(t->t_arg);
    }
    poll_depth--;
}

#define WAKE_N 32
static const volatile void* wake_ids[WAKE_N];
static unsigned int wake_cnt;
void wakeup(const volatile void *ident) { wake_ids[wake_cnt++ % WAKE_N] = ident; }

int tsleep_nsec(const volatile void *ident, int prio, const char *wmesg, uint64_t nsecs) {
    unsigned int start = wake_cnt, t0 = get_uptime_ms(), lim;
    unsigned int ms = (nsecs == (uint64_t)-1) ? 60000u : (unsigned int)(nsecs / 1000000u);
    lim = ms ? ms : 1;
    for (;;) {
        unsigned int i;
        bsd_poll();
        for (i = start; i != wake_cnt; i++) if (wake_ids[i % WAKE_N] == ident) return 0;
        if (get_uptime_ms() - t0 >= lim) return EWOULDBLOCK;
        __asm volatile("hlt");
    }
}

/* run func immediately: by the time the Wi-Fi code attaches, the root filesystem is ready */
void config_mountroot(struct device *dev, void (*func)(struct device *)) { func(dev); }

/* ---------------------------------------------------------- firmware */

static const struct { const char *bsd; const char *intel; } fw_alias[] = {
    { "iwm-9260-46",     "iwlwifi-9260-th-b0-jf-b0-46.ucode" },
    { "iwm-9000-46",     "iwlwifi-9000-pu-b0-jf-b0-46.ucode" },
    { "iwm-8265-36",     "iwlwifi-8265-36.ucode" },
    { "iwm-8000C-36",    "iwlwifi-8000C-36.ucode" },
    { "iwm-7265D-29",    "iwlwifi-7265D-29.ucode" },
    { "iwm-7265-17",     "iwlwifi-7265-17.ucode" },
    { "iwm-7260-17",     "iwlwifi-7260-17.ucode" },
    { "iwm-3168-29",     "iwlwifi-3168-29.ucode" },
    { "iwm-3160-17",     "iwlwifi-3160-17.ucode" },
};

/* firmware files live in /lib/firmware on the TanjaOS disk; the OpenBSD name or
 * Intel's own .ucode name both work */
int loadfirmware(const char *name, u_char **bufp, size_t *buflen) {
    char path[96];
    const char* tries[3] = { 0, 0, 0 };
    unsigned int i, sz;
    u_char* buf;
    for (i = 0; i < sizeof(fw_alias) / sizeof(fw_alias[0]); i++)
        if (strcmp(name, fw_alias[i].bsd) == 0) tries[1] = fw_alias[i].intel;
    tries[0] = name;
    for (i = 0; i < 2; i++) {
        if (!tries[i]) continue;
        snprintf(path, sizeof(path), "/lib/firmware/%s", tries[i]);
        if (!fs_file_exists(path)) continue;
        sz = fs_get_file_size(path);
        if (!sz) continue;
        buf = malloc(sz, M_DEVBUF, M_NOWAIT);
        if (!buf) return ENOMEM;
        if (fs_read_file(path, (char*)buf, &sz) != 0) { free(buf, M_DEVBUF, sz); return EIO; }
        *bufp = buf; *buflen = sz;
        return 0;
    }
    return ENOENT;
}

/* ------------------------------------------------------------ selftest */

static volatile int st_flag;
static void st_cb(void *a) { *(volatile int*)a = 1; }
static struct task st_task;
static struct timeout st_to;

#define CHK(c, msg) do { if (c) pass++; else { fail++; printf("  FAIL: %s\n", msg); } } while (0)
int bsd_selftest(void) {
    int pass = 0, fail = 0;
    void* a; void* b; char* z; unsigned int t0;
    volatile int tflag = 0, kflag = 0;

    a = malloc(100, M_DEVBUF, M_NOWAIT); b = malloc(100, M_DEVBUF, M_NOWAIT);
    CHK(a && b && a != b, "malloc distinct blocks");
    free(a, M_DEVBUF, 100);
    CHK(malloc(100, M_DEVBUF, M_NOWAIT) == a, "free list reuses blocks");
    z = malloc(5000, M_DEVBUF, M_NOWAIT | M_ZERO);
    CHK(z && z[0] == 0 && z[4999] == 0, "malloc M_ZERO");

    task_set(&st_task, st_cb, (void*)&tflag);
    CHK(task_add(systq, &st_task) == 1 && task_add(systq, &st_task) == 0, "task_add / double add");
    bsd_poll();
    CHK(tflag == 1, "task runs from bsd_poll");
    CHK(task_add(systq, &st_task) == 1 && task_del(systq, &st_task) == 1, "task_del");

    timeout_set(&st_to, st_cb, (void*)&kflag);
    timeout_add_msec(&st_to, 30);
    bsd_poll();
    CHK(kflag == 0 && timeout_pending(&st_to), "timeout not yet due");
    t0 = get_uptime_ms();
    while (get_uptime_ms() - t0 < 60) { bsd_poll(); __asm volatile("hlt"); }
    CHK(kflag == 1 && !timeout_pending(&st_to), "timeout fires once, after its delay");
    timeout_add_msec(&st_to, 1000); CHK(timeout_del(&st_to) == 1 && timeout_del(&st_to) == 0, "timeout_del");

    t0 = get_uptime_ms();
    CHK(tsleep_nsec(&st_flag, 0, "t", 50 * 1000000ull) == EWOULDBLOCK && get_uptime_ms() - t0 >= 45, "tsleep times out");
    st_flag = 0; task_set(&st_task, (void(*)(void*))wakeup, (void*)&st_flag); task_add(systq, &st_task);
    CHK(tsleep_nsec(&st_flag, 0, "t", 500 * 1000000ull) == 0, "tsleep woken by a task calling wakeup");

    { uint32_t x = arc4random(), y = arc4random(); CHK(x != y, "arc4random varies"); }
    { int i, ok = 1; for (i = 0; i < 200; i++) if (arc4random_uniform(7) >= 7) ok = 0; CHK(ok, "arc4random_uniform range"); }
    { unsigned char d[3] = { 1, 2, 3 }; CHK(timingsafe_bcmp(d, d, 3) == 0, "timingsafe_bcmp"); }
    { u_char* fb; size_t fl; CHK(loadfirmware("iwm-nonexistent", &fb, &fl) == ENOENT, "loadfirmware missing file -> ENOENT"); }
    if (fs_file_exists("/lib/firmware/iwlwifi-9260-th-b0-jf-b0-46.ucode")) {      /* only when you installed it */
        u_char* fb; size_t fl;
        unsigned int want = fs_get_file_size("/lib/firmware/iwlwifi-9260-th-b0-jf-b0-46.ucode");
        CHK(loadfirmware("iwm-9260-46", &fb, &fl) == 0 && fl == want && want > 0, "loadfirmware finds the 9260 firmware under its Intel file name");
        printf("  (9260 firmware present: %u bytes)\n", want);
    }

    printf("bsd selftest (core): %d passed, %d failed\n", pass, fail);
    return fail;
}
