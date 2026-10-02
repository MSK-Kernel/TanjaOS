#ifndef TANJA_UTF8_H
#define TANJA_UTF8_H

#include <stdint.h>

/* ============================================================
 * SHARED UTF-8 SUPPORT
 * ------------------------------------------------------------
 * TanjaOS runs on the VGA text console, which is byte-oriented
 * (CP437 glyphs).  Real text files, however, contain UTF-8, so
 * punctuation like “ ” — ʼ used to render as two or three
 * jumbled CP437 characters (one per byte).  These helpers let
 * the kernel console and the editor decode UTF-8 sequences and
 * draw a single sensible ASCII cell instead.
 * ============================================================ */

#define UTF8_INVALID 0xFFFFFFFFu

/* Decode one UTF-8 sequence starting at s, where at most `len`
 * bytes are available (the buffer may continue past `len`).
 * Stores the codepoint in *cp, or UTF8_INVALID if the sequence
 * is malformed.  Returns the number of bytes consumed (>= 1). */
static int utf8_decode(const char *s, int len, uint32_t *cp)
{
    unsigned char b0 = (unsigned char)s[0];
    uint32_t c;
    int need, i;

    *cp = UTF8_INVALID;
    if (b0 < 0x80) { *cp = b0; return 1; }
    if ((b0 & 0xE0) == 0xC0) {
        c = b0 & 0x1F;
        need = 1;
        if (c < 2) return 1;             /* overlong C0/C1 lead */
    } else if ((b0 & 0xF0) == 0xE0) {
        c = b0 & 0x0F;
        need = 2;
    } else if ((b0 & 0xF8) == 0xF0) {
        c = b0 & 0x07;
        need = 3;
    } else {
        return 1;                        /* lone continuation / bad lead */
    }

    if (len < need + 1) return 1;       /* truncated at end of buffer */

    for (i = 1; i <= need; i++) {
        unsigned char b = (unsigned char)s[i];
        if ((b & 0xC0) != 0x80) return 1;
        c = (c << 6) | (b & 0x3F);
    }

    if ((need == 2 && c < 0x800) ||      /* overlong */
        (need == 3 && c < 0x10000) ||    /* overlong */
        (c >= 0xD800 && c <= 0xDFFF) ||  /* surrogate */
        c > 0x10FFFF)
        return 1;

    *cp = c;
    return need + 1;
}

/* ASCII replacement for Unicode punctuation that commonly appears
 * in text files.  Returns 0 when there is no mapping. */
static int utf8_map_ascii(uint32_t cp)
{
    switch (cp) {
    case 0x2018: /* left single quote */
    case 0x2019: /* right single quote */
    case 0x201A: /* low single quote */
    case 0x02BC: /* modifier apostrophe */
        return '\'';
    case 0x201C: /* left double quote */
    case 0x201D: /* right double quote */
        return '"';
    case 0x2013: /* en dash */
    case 0x2014: /* em dash */
        return '-';
    case 0x2026: /* ellipsis */
        return '.';
    default:
        return 0;
    }
}

/* Console cell for a decoded codepoint: ASCII passes through,
 * mapped punctuation is transliterated, everything else renders
 * as '?'.  Returns 0 for UTF8_INVALID (caller draws raw bytes). */
static char utf8_to_cell(uint32_t cp)
{
    int m;
    if (cp == UTF8_INVALID) return 0;
    if (cp < 0x80) return (char)cp;
    m = utf8_map_ascii(cp);
    return m ? (char)m : '?';
}

/* Incremental byte-at-a-time decoder for streaming output. */
typedef struct {
    char buf[4];
    int  len;    /* bytes buffered so far */
    int  need;   /* continuation bytes still expected */
} utf8_feed_t;

/* Feed one byte to the state machine.
 * Returns: 0 -> byte consumed, sequence still in progress
 *          1 -> *cp holds a complete codepoint
 *          2 -> byte is not valid UTF-8 on its own (state reset);
 *               the caller should draw this raw byte like the
 *               old byte-oriented behavior did. */
static int utf8_feed(utf8_feed_t *st, unsigned char c, uint32_t *cp)
{
    *cp = UTF8_INVALID;

    if (st->need > 0) {
        if ((c & 0xC0) == 0x80 && st->len < 4) {
            st->buf[st->len++] = (char)c;
            if (--st->need == 0) {
                utf8_decode(st->buf, st->len, cp);
                st->len = 0;
                return 1;
            }
            return 0;
        }
        /* Broken sequence: drop what was buffered and reprocess
         * this byte from a clean state. */
        st->len = 0;
        st->need = 0;
    }

    if (c < 0x80) { *cp = c; return 1; }
    if ((c & 0xE0) == 0xC0) { st->buf[0] = (char)c; st->len = 1; st->need = 1; return 0; }
    if ((c & 0xF0) == 0xE0) { st->buf[0] = (char)c; st->len = 1; st->need = 2; return 0; }
    if ((c & 0xF8) == 0xF0) { st->buf[0] = (char)c; st->len = 1; st->need = 3; return 0; }
    return 2; /* lone continuation byte or invalid lead */
}

#endif /* TANJA_UTF8_H */
