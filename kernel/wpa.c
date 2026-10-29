/* wpa.c - WPA2-PSK / CCMP core, independent of any Wi-Fi chip.
 *
 * Built on BearSSL's SHA-1, HMAC and AES. Covers: passphrase -> PMK
 * (PBKDF2), PTK derivation, the supplicant side of the 4-way handshake and
 * the group-key handshake (EAPOL-Key, descriptor version 2 = HMAC-SHA1-128 +
 * AES key wrap), RFC 3394 unwrap, and generic AES-CCM.
 *
 * Not here (driver step): 802.11 frame building, building the CCMP AAD/nonce
 * from a real 802.11 header, replay checking of data frames, TKIP, WPA3/SAE.
 */
#include <stdint.h>
#include <stddef.h>
#include "bearssl.h"
#include "../include/wpa.h"

/* ------------------------------------------------------------ helpers */

static int ct_eq(const uint8_t* a, const uint8_t* b, size_t n) {
    uint8_t d = 0; size_t i;
    for (i = 0; i < n; i++) d |= a[i] ^ b[i];
    return d == 0;
}

static void hmac_sha1(const uint8_t* key, size_t klen, const uint8_t* const* parts,
                      const size_t* lens, int np, uint8_t out[20]) {
    br_hmac_key_context kc;
    br_hmac_context hc;
    int i;
    br_hmac_key_init(&kc, &br_sha1_vtable, key, klen);
    br_hmac_init(&hc, &kc, 0);
    for (i = 0; i < np; i++) br_hmac_update(&hc, parts[i], lens[i]);
    br_hmac_out(&hc, out);
}

static uint64_t be64(const uint8_t* p) {
    uint64_t v = 0; int i;
    for (i = 0; i < 8; i++) v = (v << 8) | p[i];
    return v;
}

/* ------------------------------------------------------- PMK / PTK */

void wpa_pmk_from_passphrase(const char* pass, const char* ssid, uint8_t pmk[32]) {
    br_hmac_key_context kc;
    br_hmac_context hc;
    uint8_t u[20], t[20], salt_i[4];
    int block, i, it, got = 0;
    int slen = strlen(ssid);
    br_hmac_key_init(&kc, &br_sha1_vtable, pass, strlen(pass));
    for (block = 1; got < 32; block++) {
        salt_i[0] = 0; salt_i[1] = 0; salt_i[2] = 0; salt_i[3] = (uint8_t)block;
        br_hmac_init(&hc, &kc, 0);
        br_hmac_update(&hc, ssid, slen);
        br_hmac_update(&hc, salt_i, 4);
        br_hmac_out(&hc, u);
        memcpy(t, u, 20);
        for (it = 1; it < 4096; it++) {
            br_hmac_init(&hc, &kc, 0);
            br_hmac_update(&hc, u, 20);
            br_hmac_out(&hc, u);
            for (i = 0; i < 20; i++) t[i] ^= u[i];
        }
        for (i = 0; i < 20 && got < 32; i++) pmk[got++] = t[i];
    }
}

static int memcmp_(const uint8_t* a, const uint8_t* b, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

void wpa_derive_ptk(const uint8_t pmk[32], const uint8_t aa[6], const uint8_t spa[6],
                    const uint8_t anonce[32], const uint8_t snonce[32], uint8_t ptk[48]) {
    static const char label[] = "Pairwise key expansion";
    uint8_t data[76];                 /* min(AA,SPA) max(AA,SPA) min(N) max(N) */
    uint8_t out[40];
    int i, n = 0;
    if (memcmp_(aa, spa, 6) < 0) { memcpy(data, aa, 6); memcpy(data + 6, spa, 6); }
    else                         { memcpy(data, spa, 6); memcpy(data + 6, aa, 6); }
    if (memcmp_(anonce, snonce, 32) < 0) { memcpy(data + 12, anonce, 32); memcpy(data + 44, snonce, 32); }
    else                                 { memcpy(data + 12, snonce, 32); memcpy(data + 44, anonce, 32); }
    for (i = 0; n < 48; i++) {        /* PRF-384: HMAC-SHA1(PMK, label || 0 || data || i) */
        uint8_t zero = 0, ctr = (uint8_t)i;
        const uint8_t* parts[4] = { (const uint8_t*)label, &zero, data, &ctr };
        size_t lens[4] = { sizeof(label) - 1, 1, sizeof(data), 1 };
        hmac_sha1(pmk, 32, parts, lens, 4, out);
        memcpy(ptk + n, out, (48 - n) < 20 ? (48 - n) : 20);
        n += 20;
    }
}

/* ------------------------------------------------------- AES + wrap */

static void aes_enc_block(const br_aes_ct_cbcenc_keys* k, const uint8_t in[16], uint8_t out[16]) {
    uint8_t iv[16] = { 0 };
    memcpy(out, in, 16);
    br_aes_ct_cbcenc_run(k, iv, out, 16);
}

int wpa_aes_unwrap(const uint8_t kek[16], const uint8_t* in, size_t in_len, uint8_t* out) {
    br_aes_ct_cbcdec_keys dk;
    uint8_t a[8], b[16], iv[16];
    size_t n, i;
    int j;
    if (in_len < 16 || (in_len & 7)) return -1;
    n = in_len / 8 - 1;
    br_aes_ct_cbcdec_init(&dk, kek, 16);
    memcpy(a, in, 8);
    memcpy(out, in + 8, n * 8);
    for (j = 5; j >= 0; j--) {
        for (i = n; i >= 1; i--) {
            uint64_t t = (uint64_t)n * j + i;
            int k2;
            for (k2 = 0; k2 < 8; k2++) b[k2] = a[k2] ^ (uint8_t)(t >> (56 - 8 * k2));
            memcpy(b + 8, out + (i - 1) * 8, 8);
            memset(iv, 0, 16);
            br_aes_ct_cbcdec_run(&dk, iv, b, 16);          /* one block, zero IV == plain AES decrypt */
            memcpy(a, b, 8);
            memcpy(out + (i - 1) * 8, b + 8, 8);
        }
    }
    for (i = 0; i < 8; i++) if (a[i] != 0xA6) return -1;
    return 0;
}

/* ---------------------------------------------------------- AES-CCM */

static void ccm_mac_block(const br_aes_ct_cbcenc_keys* k, uint8_t x[16], const uint8_t* d, size_t n) {
    uint8_t blk[16]; size_t i;
    memset(blk, 0, 16);
    for (i = 0; i < n; i++) blk[i] = d[i];
    for (i = 0; i < 16; i++) x[i] ^= blk[i];
    aes_enc_block(k, x, x);
}

static void ccm_tag(const br_aes_ct_cbcenc_keys* k, const uint8_t nonce[13], const uint8_t* aad,
                    size_t aad_len, const uint8_t* msg, size_t len, uint8_t t[16]) {
    uint8_t b[16], x[16];
    size_t i;
    b[0] = (aad_len ? 0x40 : 0) | (((8 - 2) / 2) << 3) | (2 - 1);
    memcpy(b + 1, nonce, 13);
    b[14] = (uint8_t)(len >> 8); b[15] = (uint8_t)len;
    aes_enc_block(k, b, x);
    if (aad_len) {
        uint8_t first[16];
        size_t take = aad_len < 14 ? aad_len : 14;
        memset(first, 0, 16);
        first[0] = (uint8_t)(aad_len >> 8); first[1] = (uint8_t)aad_len;
        memcpy(first + 2, aad, take);
        for (i = 0; i < 16; i++) x[i] ^= first[i];
        aes_enc_block(k, x, x);
        for (i = take; i < aad_len; i += 16) ccm_mac_block(k, x, aad + i, aad_len - i < 16 ? aad_len - i : 16);
    }
    for (i = 0; i < len; i += 16) ccm_mac_block(k, x, msg + i, len - i < 16 ? len - i : 16);
    memcpy(t, x, 16);
}

static void ccm_ctr(const br_aes_ct_cbcenc_keys* k, const uint8_t nonce[13], uint32_t ctr, uint8_t s[16]) {
    uint8_t a[16];
    a[0] = 2 - 1;
    memcpy(a + 1, nonce, 13);
    a[14] = (uint8_t)(ctr >> 8); a[15] = (uint8_t)ctr;
    aes_enc_block(k, a, s);
}

static void ccm_xor_stream(const br_aes_ct_cbcenc_keys* k, const uint8_t nonce[13],
                           const uint8_t* in, size_t len, uint8_t* out) {
    uint8_t s[16]; size_t i, j; uint32_t c = 1;
    for (i = 0; i < len; i += 16, c++) {
        size_t n = len - i < 16 ? len - i : 16;
        ccm_ctr(k, nonce, c, s);
        for (j = 0; j < n; j++) out[i + j] = in[i + j] ^ s[j];
    }
}

void wpa_ccm_encrypt(const uint8_t key[16], const uint8_t nonce[13], const uint8_t* aad, size_t aad_len,
                     const uint8_t* plain, size_t len, uint8_t* cipher, uint8_t tag[8]) {
    br_aes_ct_cbcenc_keys k;
    uint8_t t[16], s0[16];
    int i;
    br_aes_ct_cbcenc_init(&k, key, 16);
    ccm_tag(&k, nonce, aad, aad_len, plain, len, t);
    ccm_ctr(&k, nonce, 0, s0);
    for (i = 0; i < 8; i++) tag[i] = t[i] ^ s0[i];
    ccm_xor_stream(&k, nonce, plain, len, cipher);
}

int wpa_ccm_decrypt(const uint8_t key[16], const uint8_t nonce[13], const uint8_t* aad, size_t aad_len,
                    const uint8_t* cipher, size_t len, const uint8_t tag[8], uint8_t* plain) {
    br_aes_ct_cbcenc_keys k;
    uint8_t t[16], s0[16], mine[8];
    int i;
    br_aes_ct_cbcenc_init(&k, key, 16);
    ccm_xor_stream(&k, nonce, cipher, len, plain);
    ccm_tag(&k, nonce, aad, aad_len, plain, len, t);
    ccm_ctr(&k, nonce, 0, s0);
    for (i = 0; i < 8; i++) mine[i] = t[i] ^ s0[i];
    if (!ct_eq(mine, tag, 8)) { memset(plain, 0, len); return -1; }
    return 0;
}

/* ------------------------------------------------- EAPOL-Key handshake */

#define EK_HDR      4
#define EK_TYPE     4
#define EK_INFO     5
#define EK_REPLAY   9
#define EK_NONCE    17
#define EK_MIC      81
#define EK_DLEN     97
#define EK_DATA     99

#define KI_VER_MASK 0x0007
#define KI_PAIRWISE 0x0008
#define KI_INSTALL  0x0040
#define KI_ACK      0x0080
#define KI_MIC      0x0100
#define KI_SECURE   0x0200
#define KI_ENCDATA  0x1000

static uint16_t rd16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static void     wr16(uint8_t* p, uint16_t v) { p[0] = v >> 8; p[1] = (uint8_t)v; }

static void eapol_mic(const uint8_t kck[16], const uint8_t* frame, size_t len, uint8_t mic[16]) {
    uint8_t copy[512], full[20];
    const uint8_t* parts[1];
    size_t lens[1];
    memcpy(copy, frame, len);
    memset(copy + EK_MIC, 0, 16);
    parts[0] = copy; lens[0] = len;
    hmac_sha1(kck, 16, parts, lens, 1, full);
    memcpy(mic, full, 16);
}

static int check_mic(const wpa_sup_t* s, const uint8_t* f, size_t len) {
    uint8_t mic[16];
    if (len > 512) return 0;
    eapol_mic(s->ptk, f, len, mic);
    return ct_eq(mic, f + EK_MIC, 16);
}

/* build + MIC an outgoing EAPOL-Key (msg2 / msg4 / group msg2) */
static size_t build_reply(const wpa_sup_t* s, uint16_t key_info, uint64_t replay,
                          const uint8_t* nonce, const uint8_t* data, size_t dlen, uint8_t* out) {
    size_t total = EK_DATA + dlen;
    int i;
    memset(out, 0, total);
    out[0] = 2;                       /* EAPOL version 802.1X-2004 */
    out[1] = 3;                       /* EAPOL-Key */
    wr16(out + 2, (uint16_t)(total - EK_HDR));
    out[EK_TYPE] = 2;                 /* RSN key descriptor */
    wr16(out + EK_INFO, key_info);
    for (i = 0; i < 8; i++) out[EK_REPLAY + i] = (uint8_t)(replay >> (56 - 8 * i));
    if (nonce) memcpy(out + EK_NONCE, nonce, 32);
    wr16(out + EK_DLEN, (uint16_t)dlen);
    if (dlen) memcpy(out + EK_DATA, data, dlen);
    {
        uint8_t mic[16];
        eapol_mic(s->ptk, out, total, mic);
        memcpy(out + EK_MIC, mic, 16);
    }
    return total;
}

/* find the GTK KDE (dd len 00-0f-ac 01 | keyid | rsvd | gtk) in decrypted key data */
static int find_gtk(const uint8_t* d, size_t n, int* id, const uint8_t** gtk, size_t* glen) {
    size_t i = 0;
    while (i + 2 <= n) {
        uint8_t t = d[i], l = d[i + 1];
        if (t == 0) break;                           /* padding (0xdd 00 .. handled by length) */
        if (i + 2 + l > n) return 0;
        if (t == 0xdd && l >= 6 + 16 && d[i + 2] == 0x00 && d[i + 3] == 0x0f && d[i + 4] == 0xac && d[i + 5] == 1) {
            *id = d[i + 6] & 3;
            *gtk = d + i + 8;
            *glen = l - 6;
            return 1;
        }
        i += 2 + l;
    }
    return 0;
}

void wpa_sup_init(wpa_sup_t* s, const wpa_ops_t* ops, const uint8_t pmk[32],
                  const uint8_t aa[6], const uint8_t spa[6], const uint8_t* rsn_ie, size_t rsn_ie_len) {
    memset(s, 0, sizeof(*s));
    s->ops = ops;
    memcpy(s->pmk, pmk, 32);
    memcpy(s->aa, aa, 6);
    memcpy(s->spa, spa, 6);
    s->rsn_ie = rsn_ie;
    s->rsn_ie_len = rsn_ie_len;
}

int wpa_sup_rx_eapol(wpa_sup_t* s, const uint8_t* f, size_t len, uint8_t* reply, size_t* reply_len) {
    uint16_t info;
    uint64_t rc;
    size_t dlen;
    *reply_len = 0;
    if (len < EK_DATA || f[1] != 3 || f[EK_TYPE] != 2) return WPA_E_BAD;
    if ((size_t)rd16(f + 2) + EK_HDR > len) return WPA_E_BAD;
    len = (size_t)rd16(f + 2) + EK_HDR;
    info = rd16(f + EK_INFO);
    rc = be64(f + EK_REPLAY);
    dlen = rd16(f + EK_DLEN);
    if (EK_DATA + dlen > len) return WPA_E_BAD;
    if ((info & KI_VER_MASK) != 2) return WPA_E_UNSUPP;       /* TKIP (1) / SHA-256 (3) */

    if ((info & KI_PAIRWISE) && (info & KI_ACK) && !(info & KI_MIC)) {          /* msg 1 */
        memcpy(s->anonce, f + EK_NONCE, 32);
        s->ops->random(s->snonce, 32);
        wpa_derive_ptk(s->pmk, s->aa, s->spa, s->anonce, s->snonce, s->ptk);
        s->replay = rc; s->have_replay = 1;
        s->state = WPA_ST_GOT_MSG1;
        *reply_len = build_reply(s, 0x010a, rc, s->snonce, s->rsn_ie, s->rsn_ie_len, reply);
        return WPA_REPLY;
    }

    if ((info & KI_PAIRWISE) && (info & KI_ACK) && (info & KI_MIC) && (info & KI_INSTALL)) {   /* msg 3 */
        uint8_t data[400];
        int gid = -1; const uint8_t* gtk = 0; size_t glen = 0;
        if (s->state != WPA_ST_GOT_MSG1 && s->state != WPA_ST_DONE) return WPA_E_STATE;
        if (!check_mic(s, f, len)) return WPA_E_MIC;
        if (s->have_replay && rc <= s->replay) return WPA_E_REPLAY;
        if (!ct_eq(f + EK_NONCE, s->anonce, 32)) return WPA_E_BAD;     /* ANonce must not change */
        if (!(info & KI_ENCDATA) || dlen < 16 || dlen > sizeof(data) + 8) return WPA_E_BAD;
        if (wpa_aes_unwrap(s->ptk + 16, f + EK_DATA, dlen, data) != 0) return WPA_E_MIC;
        s->replay = rc;
        if (find_gtk(data, dlen - 8, &gid, &gtk, &glen)) s->ops->install_gtk(gid, gtk, glen);
        s->ops->install_ptk(s->ptk + 32);
        s->state = WPA_ST_DONE;
        *reply_len = build_reply(s, 0x030a, rc, 0, 0, 0, reply);
        return WPA_REPLY;
    }

    if (!(info & KI_PAIRWISE) && (info & KI_ACK) && (info & KI_MIC) && (info & KI_ENCDATA)) {  /* group msg 1 */
        uint8_t data[400];
        int gid = -1; const uint8_t* gtk = 0; size_t glen = 0;
        if (s->state != WPA_ST_DONE) return WPA_E_STATE;
        if (!check_mic(s, f, len)) return WPA_E_MIC;
        if (rc <= s->replay) return WPA_E_REPLAY;
        if (dlen < 16 || dlen > sizeof(data) + 8) return WPA_E_BAD;
        if (wpa_aes_unwrap(s->ptk + 16, f + EK_DATA, dlen, data) != 0) return WPA_E_MIC;
        s->replay = rc;
        if (find_gtk(data, dlen - 8, &gid, &gtk, &glen)) s->ops->install_gtk(gid, gtk, glen);
        *reply_len = build_reply(s, 0x0302, rc, 0, 0, 0, reply);
        return WPA_REPLY;
    }
    return WPA_E_BAD;
}
