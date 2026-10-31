/* bsd_mbuf.c - mbuf chains for the OpenBSD Wi-Fi code.
 * Same struct layout and semantics as OpenBSD's, simplified: clusters are never
 * shared (m_copym deep-copies), so there is no reference counting on external storage. */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/errno.h>

extern int printf(const char* fmt, ...);

static struct mbuf *mbuf_alloc(int type, int hdr) {
    struct mbuf *m = malloc(MSIZE, M_DEVBUF, M_NOWAIT | M_ZERO);
    if (!m) return NULL;
    m->m_type = type;
    if (hdr) {
        m->m_flags = M_PKTHDR;
        m->m_data = m->m_pktdat;
    } else {
        m->m_data = m->m_dat;
    }
    return m;
}

struct mbuf *m_get(int how, int type) { return mbuf_alloc(type, 0); }
struct mbuf *m_gethdr(int how, int type) { return mbuf_alloc(type, 1); }

/* attach an external buffer of at least pktlen bytes (at least MCLBYTES) */
struct mbuf *m_clget(struct mbuf *m, int how, u_int pktlen) {
    u_int size = pktlen > MCLBYTES ? pktlen : MCLBYTES;
    caddr_t buf;
    if (m == NULL) { m = m_get(how, MT_DATA); if (m == NULL) return NULL; }
    if (m->m_flags & M_EXT) return m;
    buf = malloc(size, M_DEVBUF, M_NOWAIT);
    if (!buf) return m;
    m->m_data = m->m_ext.ext_buf = buf;
    m->m_ext.ext_size = size;
    m->m_ext.ext_free_fn = 0;
    m->m_ext.ext_arg = NULL;
    m->m_flags |= M_EXT | M_EXTWR;
    return m;
}

static void ext_free(struct mbuf *m) {
    if (m->m_flags & M_EXT) {
        if (m->m_ext.ext_free_fn == 0) free(m->m_ext.ext_buf, M_DEVBUF, m->m_ext.ext_size);
        m->m_flags &= ~(M_EXT | M_EXTWR);
    }
}

struct mbuf *m_free(struct mbuf *m) {
    struct mbuf *n;
    if (!m) return NULL;
    n = m->m_next;
    ext_free(m);
    free(m, M_DEVBUF, MSIZE);
    return n;
}

struct mbuf *m_freem(struct mbuf *m) { while (m) m = m_free(m); return NULL; }

/* storage bounds of one mbuf */
static caddr_t m_start(struct mbuf *m) {
    if (m->m_flags & M_EXT) return m->m_ext.ext_buf;
    if (m->m_flags & M_PKTHDR) return m->m_pktdat;
    return m->m_dat;
}
static u_int m_size(struct mbuf *m) {
    if (m->m_flags & M_EXT) return m->m_ext.ext_size;
    if (m->m_flags & M_PKTHDR) return MHLEN;
    return MLEN;
}

int m_trailingspace(struct mbuf *m) { return (int)(m_start(m) + m_size(m) - (m->m_data + m->m_len)); }

void m_align(struct mbuf *m, int len) {
    int adj = (int)m_size(m) - len;
    m->m_data = m_start(m) + (adj & ~(int)(sizeof(long) - 1));
}

int m_dup_pkthdr(struct mbuf *to, struct mbuf *from, int wait) {
    to->m_flags = (to->m_flags & (M_EXT | M_EXTWR)) | (from->m_flags & ~(M_EXT | M_EXTWR));
    to->m_pkthdr = from->m_pkthdr;
    return 0;
}

void m_adj(struct mbuf *mp, int req_len) {
    int len = req_len;
    struct mbuf *m = mp;
    int count;
    if (!m) return;
    if (len >= 0) {
        while (m && len > 0) {
            if (m->m_len <= len) { len -= m->m_len; m->m_len = 0; m = m->m_next; }
            else { m->m_len -= len; m->m_data += len; len = 0; }
        }
        if (mp->m_flags & M_PKTHDR) mp->m_pkthdr.len -= (req_len - len);
    } else {
        len = -len;
        count = 0;
        for (;;) { count += m->m_len; if (m->m_next == NULL) break; m = m->m_next; }
        if (m->m_len >= len) { m->m_len -= len; }
        else {
            count -= len;
            if (count < 0) count = 0;
            m = mp;
            if (m->m_flags & M_PKTHDR) m->m_pkthdr.len = count;
            for (; m; m = m->m_next) {
                if (m->m_len >= count) {
                    m->m_len = count;
                    while ((m = m->m_next) != NULL) m->m_len = 0;
                    break;
                }
                count -= m->m_len;
            }
            return;
        }
        if (mp->m_flags & M_PKTHDR) mp->m_pkthdr.len -= len;
    }
}

void m_copydata(struct mbuf *m, int off, int len, void *cp) {
    caddr_t dst = cp;
    while (off > 0 && m) { if (off < m->m_len) break; off -= m->m_len; m = m->m_next; }
    while (len > 0 && m) {
        int n = m->m_len - off;
        if (n > len) n = len;
        memcpy(dst, m->m_data + off, n);
        dst += n; len -= n; off = 0; m = m->m_next;
    }
}

static struct mbuf *copy_new(struct mbuf *m0, int off, int len, int hdr, int lead) {
    struct mbuf *n = hdr ? m_gethdr(M_DONTWAIT, MT_DATA) : m_get(M_DONTWAIT, MT_DATA);
    if (!n) return NULL;
    if ((u_int)(len + lead) > (hdr ? MHLEN : MLEN)) {
        m_clget(n, M_DONTWAIT, len + lead);
        if (!(n->m_flags & M_EXT)) { m_free(n); return NULL; }
    }
    n->m_data = m_start(n) + lead;
    m_copydata(m0, off, len, n->m_data);
    n->m_len = len;
    return n;
}

/* deep copy (no shared clusters) */
struct mbuf *m_copym(struct mbuf *m, int off, int len, int wait) {
    struct mbuf *top = NULL, **np = &top;
    if (len == M_COPYALL) {
        struct mbuf *t; int tot = 0;
        for (t = m; t; t = t->m_next) tot += t->m_len;
        len = tot - off;
        if (len < 0) len = 0;
    }
    if (off == 0 && (m->m_flags & M_PKTHDR) && len > 0) {
        top = copy_new(m, off, len > MCLBYTES * 4 ? MCLBYTES * 4 : len, 1, 0);
        if (!top) return NULL;
        m_dup_pkthdr(top, m, wait);
        /* continuation (very large packets) */
        len -= top->m_len; off += top->m_len; np = &top->m_next;
    }
    while (len > 0) {
        int n = len > MCLBYTES * 4 ? MCLBYTES * 4 : len;
        struct mbuf *c = copy_new(m, off, n, 0, 0);
        if (!c) { m_freem(top); return NULL; }
        *np = c; np = &c->m_next; len -= n; off += n;
    }
    if (!top) top = m_get(wait, MT_DATA);
    return top;
}

struct mbuf *m_dup_pkt(struct mbuf *m0, unsigned int adj, int wait) {
    int len = m0->m_pkthdr.len;
    struct mbuf *m = copy_new(m0, 0, len, 1, adj);
    if (!m) return NULL;
    m_dup_pkthdr(m, m0, wait);
    m->m_pkthdr.len = len;
    return m;
}

/* coalesce the chain into the first mbuf */
int m_defrag(struct mbuf *m, int how) {
    int total = 0, off = 0;
    struct mbuf *t;
    caddr_t buf;
    if (m->m_next == NULL) return 0;
    for (t = m; t; t = t->m_next) total += t->m_len;
    if ((u_int)total <= ((m->m_flags & M_PKTHDR) ? MHLEN : MLEN) && !(m->m_flags & M_EXT)) {
        buf = m_start(m);
    } else {
        buf = malloc(total, M_DEVBUF, M_NOWAIT);
        if (!buf) return ENOBUFS;
    }
    for (t = m; t; t = t->m_next) {
        memmove(buf + off, t->m_data, t->m_len);       /* memmove: first copy may overlap */
        off += t->m_len;
    }
    m_freem(m->m_next);
    m->m_next = NULL;
    if (buf != m_start(m)) {
        ext_free(m);
        m->m_data = m->m_ext.ext_buf = buf;
        m->m_ext.ext_size = total;
        m->m_ext.ext_free_fn = 0;
        m->m_ext.ext_arg = NULL;
        m->m_flags |= M_EXT | M_EXTWR;
    } else {
        m->m_data = buf;
    }
    m->m_len = total;
    return 0;
}

struct mbuf *m_prepend(struct mbuf *m, int len, int how) {
    struct mbuf *n;
    if (m_start(m) + 0 <= m->m_data && (m->m_data - m_start(m)) >= len && !(m->m_flags & M_EXT && !(m->m_flags & M_EXTWR))) {
        m->m_data -= len; m->m_len += len;
        if (m->m_flags & M_PKTHDR) m->m_pkthdr.len += len;
        return m;
    }
    n = (m->m_flags & M_PKTHDR) ? m_gethdr(how, m->m_type) : m_get(how, m->m_type);
    if (!n) { m_freem(m); return NULL; }
    if (m->m_flags & M_PKTHDR) {
        m_dup_pkthdr(n, m, how);
        m->m_flags &= ~M_PKTHDR;
    }
    n->m_next = m;
    n->m_data = m_start(n) + m_size(n) - len;
    n->m_data = (caddr_t)((unsigned long)n->m_data & ~(sizeof(long) - 1));
    if (n->m_data + len > m_start(n) + m_size(n)) n->m_data = m_start(n);
    n->m_len = len;
    if (n->m_flags & M_PKTHDR) n->m_pkthdr.len += len;
    return n;
}

/* make the first len bytes contiguous in the first mbuf */
struct mbuf *m_pullup(struct mbuf *n, int len) {
    struct mbuf *m;
    int count, space;
    if (len <= n->m_len && !(n->m_flags & M_EXT && 0)) return n;
    if (len > MCLBYTES) { m_freem(n); return NULL; }
    if (n->m_len >= len) return n;
    if (n->m_flags & M_PKTHDR) {
        m = m_gethdr(M_DONTWAIT, n->m_type);
        if (m) { m_dup_pkthdr(m, n, M_DONTWAIT); n->m_flags &= ~M_PKTHDR; }
    } else m = m_get(M_DONTWAIT, n->m_type);
    if (!m) { m_freem(n); return NULL; }
    if ((u_int)len > m_size(m)) {
        m_clget(m, M_DONTWAIT, len);
        if (!(m->m_flags & M_EXT)) { m_free(m); m_freem(n); return NULL; }
    }
    m->m_len = 0;
    space = (int)m_size(m);
    do {
        count = len - m->m_len;
        if (count > n->m_len) count = n->m_len;
        memcpy(m->m_data + m->m_len, n->m_data, count);
        m->m_len += count; n->m_len -= count; n->m_data += count;
        if (n->m_len == 0) n = m_free(n);
    } while (m->m_len < len && n);
    if (n) m->m_next = n;
    if (m->m_len < len) { m_freem(m); return NULL; }
    return m;
}

struct mbuf *m_split(struct mbuf *m0, int len0, int wait) {
    int total = (m0->m_flags & M_PKTHDR) ? m0->m_pkthdr.len : 0, remain;
    struct mbuf *n;
    if (!(m0->m_flags & M_PKTHDR)) return NULL;
    if (len0 >= total) return NULL;
    remain = total - len0;
    n = copy_new(m0, len0, remain, 1, 0);
    if (!n) return NULL;
    m_dup_pkthdr(n, m0, wait);
    n->m_pkthdr.len = remain;
    m_adj(m0, -remain);
    return n;
}

struct m_tag *m_tag_find(struct mbuf *m, int type, struct m_tag *t) { return NULL; }

/* ---------------------------------------------------- lists and queues */

void ml_init(struct mbuf_list *ml) { ml->ml_head = ml->ml_tail = NULL; ml->ml_len = 0; }
void ml_enqueue(struct mbuf_list *ml, struct mbuf *m) {
    m->m_nextpkt = NULL;
    if (ml->ml_tail) ml->ml_tail->m_nextpkt = m; else ml->ml_head = m;
    ml->ml_tail = m; ml->ml_len++;
}
struct mbuf *ml_dequeue(struct mbuf_list *ml) {
    struct mbuf *m = ml->ml_head;
    if (m) { ml->ml_head = m->m_nextpkt; if (!ml->ml_head) ml->ml_tail = NULL; m->m_nextpkt = NULL; ml->ml_len--; }
    return m;
}
unsigned int ml_purge(struct mbuf_list *ml) { struct mbuf *m; unsigned int n = 0; while ((m = ml_dequeue(ml)) != NULL) { m_freem(m); n++; } return n; }

void mq_init(struct mbuf_queue *mq, u_int maxlen, int ipl) { ml_init(&mq->mq_list); mq->mq_maxlen = maxlen; mq->mq_drops = 0; }
int mq_enqueue(struct mbuf_queue *mq, struct mbuf *m) {
    if (mq->mq_list.ml_len >= mq->mq_maxlen) { mq->mq_drops++; return ENOBUFS; }
    ml_enqueue(&mq->mq_list, m);
    return 0;
}
struct mbuf *mq_dequeue(struct mbuf_queue *mq) { return ml_dequeue(&mq->mq_list); }
unsigned int mq_purge(struct mbuf_queue *mq) { return ml_purge(&mq->mq_list); }

/* ------------------------------------------------------------ selftest */

#define CHK(c, msg) do { if (c) pass++; else { fail++; printf("  FAIL: %s\n", msg); } } while (0)
int bsd_mbuf_selftest(void) {
    int pass = 0, fail = 0, i;
    struct mbuf *m, *n, *c;
    unsigned char buf[3000], out[3000];
    struct mbuf_list ml; struct mbuf_queue mq;

    for (i = 0; i < (int)sizeof(buf); i++) buf[i] = (unsigned char)(i * 7 + 3);

    m = m_gethdr(M_DONTWAIT, MT_DATA);
    CHK(m && (m->m_flags & M_PKTHDR) && m->m_len == 0, "m_gethdr");
    m_clget(m, M_DONTWAIT, 2000);
    CHK(m->m_flags & M_EXT, "m_clget attaches a cluster");
    memcpy(m->m_data, buf, 1500); m->m_len = m->m_pkthdr.len = 1500;
    m_copydata(m, 100, 200, out);
    CHK(memcmp(out, buf + 100, 200) == 0, "m_copydata");

    m_adj(m, 10); CHK(m->m_len == 1490 && m->m_pkthdr.len == 1490 && (unsigned char)m->m_data[0] == buf[10], "m_adj(head)");
    m_adj(m, -90); CHK(m->m_len == 1400 && m->m_pkthdr.len == 1400, "m_adj(tail)");

    c = m_copym(m, 0, M_COPYALL, M_DONTWAIT);
    CHK(c && c->m_pkthdr.len == 1400 && memcmp(c->m_data, m->m_data, 1400) == 0 && c->m_data != m->m_data, "m_copym is an independent copy");
    m_freem(c);

    n = m_split(m, 1000, M_DONTWAIT);
    CHK(n && n->m_pkthdr.len == 400 && m->m_pkthdr.len == 1000 && (unsigned char)n->m_data[0] == buf[10 + 1000], "m_split");
    m_freem(n);

    m = m_prepend(m, 14, M_DONTWAIT);
    CHK(m && m->m_pkthdr.len == 1014 && m->m_len >= 14, "m_prepend (new mbuf in front)");
    m = m_pullup(m, 14); CHK(m && m->m_len >= 14, "m_pullup");
    CHK(m_defrag(m, M_DONTWAIT) == 0 && m->m_next == NULL && m->m_len == 1014, "m_defrag -> single mbuf");
    CHK((unsigned char)m->m_data[14] == buf[10], "payload intact after prepend/pullup/defrag");

    n = m_dup_pkt(m, 2, M_DONTWAIT);
    CHK(n && n->m_pkthdr.len == 1014 && memcmp(n->m_data, m->m_data, 1014) == 0, "m_dup_pkt");
    m_freem(n); m_freem(m);

    m = m_gethdr(M_DONTWAIT, MT_DATA);
    m_align(m, 40);
    CHK(((unsigned long)m->m_data & (sizeof(long) - 1)) == 0 && m_trailingspace(m) >= 40, "m_align leaves room, aligned");
    m_freem(m);

    ml_init(&ml);
    for (i = 0; i < 3; i++) { m = m_gethdr(M_DONTWAIT, MT_DATA); m->m_len = i + 1; ml_enqueue(&ml, m); }
    CHK(ml.ml_len == 3 && ml_dequeue(&ml)->m_len == 1 && ml.ml_len == 2, "mbuf_list FIFO");
    ml_purge(&ml); CHK(ml.ml_len == 0, "ml_purge");
    mq_init(&mq, 2, 0);
    CHK(mq_enqueue(&mq, m_get(M_DONTWAIT, MT_DATA)) == 0 && mq_enqueue(&mq, m_get(M_DONTWAIT, MT_DATA)) == 0, "mq_enqueue within limit");
    m = m_get(M_DONTWAIT, MT_DATA);
    CHK(mq_enqueue(&mq, m) == ENOBUFS && mq.mq_drops == 1, "mq_enqueue over limit drops");
    m_freem(m); mq_purge(&mq);

    printf("bsd selftest (mbuf): %d passed, %d failed\n", pass, fail);
    return fail;
}
