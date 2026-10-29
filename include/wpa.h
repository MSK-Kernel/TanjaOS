#ifndef TANJA_WPA_H
#define TANJA_WPA_H
/* WPA2-PSK (CCMP) core: chip independent. A Wi-Fi driver feeds it the EAPOL
 * frames it receives and sends the replies it produces; keys come back through
 * the callbacks. See kernel/wpa.c. */
#include <stdint.h>
#include <stddef.h>

/* passphrase (8-63 chars) + SSID -> 32-byte pairwise master key */
void wpa_pmk_from_passphrase(const char* pass, const char* ssid, uint8_t pmk[32]);

/* 802.11i PTK derivation for CCMP: ptk = KCK[16] | KEK[16] | TK[16] */
void wpa_derive_ptk(const uint8_t pmk[32], const uint8_t aa[6], const uint8_t spa[6],
                    const uint8_t anonce[32], const uint8_t snonce[32], uint8_t ptk[48]);

/* RFC 3394 AES key unwrap. in_len multiple of 8, >= 16. 0 = ok. */
int  wpa_aes_unwrap(const uint8_t kek[16], const uint8_t* in, size_t in_len, uint8_t* out);

/* AES-CCM as used by CCMP: 13-byte nonce, 8-byte tag. 0 = ok (decrypt: tag matched). */
void wpa_ccm_encrypt(const uint8_t key[16], const uint8_t nonce[13], const uint8_t* aad, size_t aad_len,
                     const uint8_t* plain, size_t len, uint8_t* cipher, uint8_t tag[8]);
int  wpa_ccm_decrypt(const uint8_t key[16], const uint8_t nonce[13], const uint8_t* aad, size_t aad_len,
                     const uint8_t* cipher, size_t len, const uint8_t tag[8], uint8_t* plain);

typedef struct {
    void (*random)(uint8_t* out, size_t n);                 /* good randomness, for the SNonce */
    void (*install_ptk)(const uint8_t tk[16]);              /* unicast key (CCMP temporal key) */
    void (*install_gtk)(int key_id, const uint8_t* gtk, size_t len);  /* group key */
} wpa_ops_t;

enum { WPA_ST_IDLE, WPA_ST_GOT_MSG1, WPA_ST_DONE };

typedef struct {
    const wpa_ops_t* ops;
    uint8_t  pmk[32];
    uint8_t  aa[6], spa[6];          /* AP and our MAC */
    const uint8_t* rsn_ie;           /* our RSN IE exactly as sent in the (re)association request */
    size_t   rsn_ie_len;
    int      state;
    uint8_t  snonce[32], anonce[32];
    uint8_t  ptk[48];
    uint64_t replay;                 /* last accepted replay counter from the AP */
    int      have_replay;
} wpa_sup_t;

#define WPA_OK          0   /* nothing to send */
#define WPA_REPLY       1   /* send *reply_len bytes of EAPOL (ethertype 0x888e) */
#define WPA_E_BAD      (-1) /* malformed / not for us */
#define WPA_E_MIC      (-2) /* MIC check failed: wrong password or attack */
#define WPA_E_REPLAY   (-3)
#define WPA_E_UNSUPP   (-4) /* TKIP / WPA3 / SHA-256 key management */
#define WPA_E_STATE    (-5)

void wpa_sup_init(wpa_sup_t* s, const wpa_ops_t* ops, const uint8_t pmk[32],
                  const uint8_t aa[6], const uint8_t spa[6], const uint8_t* rsn_ie, size_t rsn_ie_len);

/* Feed one EAPOL frame (starting at the EAPOL header, after the Ethernet header).
 * reply must hold at least 256 bytes. */
int  wpa_sup_rx_eapol(wpa_sup_t* s, const uint8_t* eapol, size_t len, uint8_t* reply, size_t* reply_len);

#endif
