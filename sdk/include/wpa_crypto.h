/*
 * wpa_crypto.h - the primitives WPA2-PSK needs on a station:
 *   SHA-1, HMAC-SHA1, PBKDF2-HMAC-SHA1 (passphrase -> PMK),
 *   the 802.11i PRF (PMK -> PTK), AES-128 and the RFC 3394 key unwrap (GTK).
 * Frame encryption itself (AES-CCMP) is done by the chip's security engine.
 * No libgcc: 32-bit arithmetic only.
 */
#ifndef WPA_CRYPTO_H
#define WPA_CRYPTO_H

#include <stdint.h>

void sha1(const uint8_t *data, uint32_t len, uint8_t out[20]);
void hmac_sha1(const uint8_t *key, uint32_t klen, const uint8_t *data, uint32_t dlen, uint8_t out[20]);
/* HMAC over several pieces (the PRF and the EAPOL MIC need that) */
void hmac_sha1_v(const uint8_t *key, uint32_t klen, const uint8_t *const *parts, const uint32_t *lens, int n,
                 uint8_t out[20]);
/* WPA2: PMK = PBKDF2(passphrase, ssid, 4096, 32) */
void wpa_pmk(const char *passphrase, const uint8_t *ssid, uint32_t ssid_len, uint8_t pmk[32]);
/* PRF-X: out_len bytes of HMAC-SHA1(key, label || 0 || data || i) */
void wpa_prf(const uint8_t *key, uint32_t klen, const char *label, const uint8_t *data, uint32_t dlen,
             uint8_t *out, uint32_t out_len);
/* PTK (48 bytes: KCK 16, KEK 16, TK 16) from PMK, addresses and nonces */
void wpa_ptk(const uint8_t pmk[32], const uint8_t aa[6], const uint8_t spa[6], const uint8_t anonce[32],
             const uint8_t snonce[32], uint8_t ptk[48]);

struct aes128 { uint32_t rk[44]; };
void aes128_init(struct aes128 *c, const uint8_t key[16]);
void aes128_encrypt(const struct aes128 *c, const uint8_t in[16], uint8_t out[16]);
void aes128_decrypt(const struct aes128 *c, const uint8_t in[16], uint8_t out[16]);
/* RFC 3394: in = (n+1)*8 bytes, out = n*8 bytes; 0 ok, -1 integrity check failed */
int  aes_key_unwrap(const uint8_t kek[16], const uint8_t *in, uint32_t in_len, uint8_t *out);

#endif
