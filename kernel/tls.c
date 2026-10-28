/* tls.c - TLS 1.2 client for TanjaOS, built on BearSSL (tools/bearssl).
 *
 * The server's certificate chain is verified against the Mozilla root
 * store in tls_anchors.h and the host name is checked. BearSSL speaks
 * TLS 1.0-1.2 (no TLS 1.3); modern servers such as GitHub still accept 1.2.
 *
 * Two honest caveats:
 *  - Entropy comes from RDRAND (when the CPU has it) plus TSC / PIT /
 *    RTC jitter. On old CPUs without RDRAND that is weaker than a real
 *    hardware RNG.
 *  - The clock is the CMOS RTC, but never earlier than the kernel's build
 *    date, so a dead RTC battery cannot make certificates look "not yet
 *    valid".
 */
#include <stdint.h>
#include <stddef.h>
#include "bearssl.h"
#include "tls_anchors.h"
#include "../include/build_id.h"

extern uint32_t get_uptime_ms(void);
extern uint8_t  inb(uint16_t port);
extern void     outb(uint16_t port, uint8_t val);

static struct {
    br_ssl_client_context  sc;
    br_x509_minimal_context xc;
    br_sslio_context       ioc;
    unsigned char          iobuf[BR_SSL_BUFSIZE_BIDI];
    int (*rd)(unsigned char*, size_t);
    int (*wr)(const unsigned char*, size_t);
    int   active;
    int   err;
} T;

/* ------------------------------------------------------------- clock */

static uint8_t cmos(uint8_t reg) { outb(0x70, reg); return inb(0x71); }

/* days since 1970-01-01 for a civil date (Howard Hinnant's algorithm) */
static int32_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int32_t)doe - 719468;
}

/* unix time from the RTC, or 0 if it looks unusable */
static uint32_t rtc_unix(void) {
    int tries, sec, min, hour, day, mon, year, b;
    for (tries = 0; tries < 5; tries++) {
        while (cmos(0x0A) & 0x80) ;                  /* update in progress */
        sec = cmos(0); min = cmos(2); hour = cmos(4);
        day = cmos(7); mon = cmos(8); year = cmos(9);
        if (!(cmos(0x0A) & 0x80)) break;
    }
    b = cmos(0x0B);
    if (!(b & 4)) {                                   /* BCD */
        sec = (sec & 15) + (sec >> 4) * 10;  min = (min & 15) + (min >> 4) * 10;
        hour = ((hour & 0x0F) + ((hour & 0x70) >> 4) * 10) | (hour & 0x80);
        day = (day & 15) + (day >> 4) * 10;  mon = (mon & 15) + (mon >> 4) * 10;
        year = (year & 15) + (year >> 4) * 10;
    }
    if (!(b & 2) && (hour & 0x80)) hour = ((hour & 0x7F) + 12) % 24;   /* 12h mode PM */
    year += 2000;
    if (mon < 1 || mon > 12 || day < 1 || day > 31 || hour > 23 || min > 59 || sec > 59) return 0;
    if (year < 2024 || year > 2099) return 0;
    return (uint32_t)days_from_civil(year, mon, day) * 86400u + hour * 3600u + min * 60u + sec;
}

/* ----------------------------------------------------------- entropy */

static int has_rdrand(void) {
    uint32_t a = 1, b, c = 0, d;
    asm volatile ("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    return (c >> 30) & 1;
}
static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    asm volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
static uint32_t pit_count(void) {
    outb(0x43, 0x00);
    uint32_t lo = inb(0x40), hi = inb(0x40);
    return (hi << 8) | lo;
}

static void seed(void) {
    unsigned char e[96];
    int i, n = 0, rr = has_rdrand();
    for (i = 0; i < 12; i++) {
        uint64_t t = rdtsc();
        uint32_t p = pit_count(), u = get_uptime_ms(), r = 0;
        if (rr) {
            int tries;
            for (tries = 0; tries < 10; tries++) {
                unsigned char ok;
                asm volatile ("rdrand %0; setc %1" : "=r"(r), "=qm"(ok));
                if (ok) break;
            }
        }
        e[n++] = (unsigned char)t;         e[n++] = (unsigned char)(t >> 8);
        e[n++] = (unsigned char)(t >> 16); e[n++] = (unsigned char)(t >> 24);
        e[n++] = (unsigned char)p;         e[n++] = (unsigned char)(p >> 8);
        e[n++] = (unsigned char)(u ^ cmos(0));
        e[n++] = (unsigned char)r;
        /* let the clocks drift against each other between samples */
        { volatile int k; for (k = 0; k < 2000 + (int)(t & 1023); k++) ; }
    }
    br_ssl_engine_inject_entropy(&T.sc.eng, e, n);
}

/* ------------------------------------------------------------ errors */

const char* tls_error_str(void) {
    switch (T.err) {
    case 0: return "connection failed";
    case BR_ERR_X509_NOT_TRUSTED:     return "certificate is not signed by a trusted root";
    case BR_ERR_X509_EXPIRED:         return "certificate expired or not yet valid (check the clock)";
    case BR_ERR_X509_BAD_SERVER_NAME: return "certificate is for a different host name";
    case BR_ERR_X509_TIME_UNKNOWN:    return "no usable clock for certificate dates";
    case BR_ERR_X509_WEAK_PUBLIC_KEY: return "certificate key is too weak";
    case BR_ERR_X509_NOT_CA: case BR_ERR_X509_FORBIDDEN_KEY_USAGE:
    case BR_ERR_X509_CRITICAL_EXTENSION: case BR_ERR_X509_BAD_SIGNATURE:
    case BR_ERR_X509_LIMIT_EXCEEDED: case BR_ERR_X509_DN_MISMATCH:
    case BR_ERR_X509_WRONG_KEY_TYPE:  return "certificate chain failed validation";
    case BR_ERR_X509_INVALID_VALUE: case BR_ERR_X509_TRUNCATED:
    case BR_ERR_X509_EMPTY_CHAIN: case BR_ERR_X509_BAD_TIME:
    case BR_ERR_X509_OVERFLOW: case BR_ERR_X509_UNSUPPORTED:
                                      return "certificate chain could not be parsed";
    case BR_ERR_IO:                   return "connection dropped during handshake";
    case BR_ERR_BAD_VERSION: case BR_ERR_UNSUPPORTED_VERSION:
                                      return "server does not offer TLS 1.2";
    case BR_ERR_BAD_PARAM: case BR_ERR_BAD_CIPHER_SUITE:
                                      return "no common cipher suite";
    }
    return "TLS handshake failed";
}

/* --------------------------------------------------------------- API */

static int cb_read(void* ctx, unsigned char* d, size_t n)        { (void)ctx; return T.rd(d, n); }
static int cb_write(void* ctx, const unsigned char* d, size_t n) { (void)ctx; return T.wr(d, n); }

/* start a session; the handshake itself runs on the first read/write */
int tls_start(const char* host, int (*rd)(unsigned char*, size_t),
              int (*wr)(const unsigned char*, size_t)) {
    uint32_t now = rtc_unix();
    if (now < (uint32_t)TANJA_BUILD_ID_LO) now = (uint32_t)TANJA_BUILD_ID_LO;
    T.rd = rd; T.wr = wr; T.err = 0; T.active = 1;
    br_ssl_client_init_full(&T.sc, &T.xc, TAs, TAs_NUM);
    br_x509_minimal_set_time(&T.xc, now / 86400 + 719528, now % 86400);
    br_ssl_engine_set_buffer(&T.sc.eng, T.iobuf, sizeof(T.iobuf), 1);
    seed();
    if (!br_ssl_client_reset(&T.sc, host, 0)) {
        T.err = br_ssl_engine_last_error(&T.sc.eng);
        T.active = 0;
        return -1;
    }
    br_sslio_init(&T.ioc, &T.sc.eng, cb_read, 0, cb_write, 0);
    return 0;
}

/* 0 = ok, <0 = failure (see tls_error_str) */
int tls_write_all(const void* b, size_t n) {
    if (!T.active) return -1;
    if (br_sslio_write_all(&T.ioc, b, n) < 0 || br_sslio_flush(&T.ioc) < 0) {
        T.err = br_ssl_engine_last_error(&T.sc.eng);
        return -1;
    }
    return 0;
}

/* >0 bytes, 0 = clean close, <0 = error */
int tls_read(void* b, size_t n) {
    int r;
    if (!T.active) return -1;
    r = br_sslio_read(&T.ioc, b, n);
    if (r > 0) return r;
    T.err = br_ssl_engine_last_error(&T.sc.eng);
    return T.err == BR_ERR_OK ? 0 : -1;
}

void tls_end(void) {
    if (T.active) br_ssl_engine_close(&T.sc.eng);
    T.active = 0;
}

int tls_last_error(void) { return T.err; }
