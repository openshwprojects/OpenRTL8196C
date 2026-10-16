/*
 * wpa_crypto.c - see wpa_crypto.h.  Straight from the standards (FIPS 180-4,
 * RFC 2104, RFC 8018, IEEE 802.11-2016 12.7.1.2, FIPS 197, RFC 3394).
 * Host-testable: tools/host_tests/wpa_crypto_test.c builds it with clang.
 */
#include "wpa_crypto.h"
#ifdef HOST_TEST
#include <string.h>
#else
#include "libc.h"
#endif

/* ------------------------------------------------------------------ SHA-1 */
static uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

static void sha1_block(uint32_t h[5], const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) | ((uint32_t)p[4 * i + 2] << 8) | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

struct sha1_ctx { uint32_t h[5]; uint8_t buf[64]; uint32_t buflen, total_lo, total_hi; };

static void sha1_init(struct sha1_ctx *c)
{
    c->h[0] = 0x67452301u; c->h[1] = 0xEFCDAB89u; c->h[2] = 0x98BADCFEu; c->h[3] = 0x10325476u; c->h[4] = 0xC3D2E1F0u;
    c->buflen = 0; c->total_lo = 0; c->total_hi = 0;
}

static void sha1_update(struct sha1_ctx *c, const uint8_t *p, uint32_t n)
{
    uint32_t lo = c->total_lo + n;
    if (lo < c->total_lo) c->total_hi++;
    c->total_lo = lo;
    while (n) {
        uint32_t k = 64 - c->buflen; if (k > n) k = n;
        memcpy(c->buf + c->buflen, p, k);
        c->buflen += k; p += k; n -= k;
        if (c->buflen == 64) { sha1_block(c->h, c->buf); c->buflen = 0; }
    }
}

static void sha1_final(struct sha1_ctx *c, uint8_t out[20])
{
    uint32_t bits_lo = c->total_lo << 3, bits_hi = (c->total_hi << 3) | (c->total_lo >> 29);
    uint8_t pad = 0x80;
    sha1_update(c, &pad, 1);
    uint8_t zero = 0;
    while (c->buflen != 56) sha1_update(c, &zero, 1);
    uint8_t len[8] = { (uint8_t)(bits_hi >> 24), (uint8_t)(bits_hi >> 16), (uint8_t)(bits_hi >> 8), (uint8_t)bits_hi,
                       (uint8_t)(bits_lo >> 24), (uint8_t)(bits_lo >> 16), (uint8_t)(bits_lo >> 8), (uint8_t)bits_lo };
    sha1_update(c, len, 8);
    for (int i = 0; i < 5; i++) {
        out[4 * i] = (uint8_t)(c->h[i] >> 24); out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(c->h[i] >> 8); out[4 * i + 3] = (uint8_t)c->h[i];
    }
}

void sha1(const uint8_t *data, uint32_t len, uint8_t out[20])
{
    struct sha1_ctx c; sha1_init(&c); sha1_update(&c, data, len); sha1_final(&c, out);
}

/* ------------------------------------------------------------- HMAC-SHA1 */
void hmac_sha1_v(const uint8_t *key, uint32_t klen, const uint8_t *const *parts, const uint32_t *lens, int n,
                 uint8_t out[20])
{
    uint8_t k[64], kh[20];
    if (klen > 64) { sha1(key, klen, kh); key = kh; klen = 20; }
    memset(k, 0, 64); memcpy(k, key, klen);
    uint8_t pad[64];
    struct sha1_ctx c;
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    sha1_init(&c); sha1_update(&c, pad, 64);
    for (int i = 0; i < n; i++) sha1_update(&c, parts[i], lens[i]);
    uint8_t inner[20]; sha1_final(&c, inner);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5C;
    sha1_init(&c); sha1_update(&c, pad, 64); sha1_update(&c, inner, 20); sha1_final(&c, out);
}

void hmac_sha1(const uint8_t *key, uint32_t klen, const uint8_t *data, uint32_t dlen, uint8_t out[20])
{
    const uint8_t *parts[1] = { data }; uint32_t lens[1] = { dlen };
    hmac_sha1_v(key, klen, parts, lens, 1, out);
}

/* ----------------------------------------------------------------- PBKDF2 */
static void pbkdf2_block(const uint8_t *pw, uint32_t pwlen, const uint8_t *salt, uint32_t slen, uint32_t iter,
                         uint32_t index, uint8_t out[20])
{
    uint8_t idx[4] = { (uint8_t)(index >> 24), (uint8_t)(index >> 16), (uint8_t)(index >> 8), (uint8_t)index };
    const uint8_t *parts[2] = { salt, idx }; uint32_t lens[2] = { slen, 4 };
    uint8_t u[20];
    hmac_sha1_v(pw, pwlen, parts, lens, 2, u);
    memcpy(out, u, 20);
    for (uint32_t i = 1; i < iter; i++) {
        hmac_sha1(pw, pwlen, u, 20, u);
        for (int k = 0; k < 20; k++) out[k] ^= u[k];
    }
}

void wpa_pmk(const char *passphrase, const uint8_t *ssid, uint32_t ssid_len, uint8_t pmk[32])
{
    uint32_t pwlen = (uint32_t)strlen(passphrase);
    uint8_t b1[20], b2[20];
    pbkdf2_block((const uint8_t *)passphrase, pwlen, ssid, ssid_len, 4096, 1, b1);
    pbkdf2_block((const uint8_t *)passphrase, pwlen, ssid, ssid_len, 4096, 2, b2);
    memcpy(pmk, b1, 20); memcpy(pmk + 20, b2, 12);
}

/* --------------------------------------------------------------- PRF-X */
void wpa_prf(const uint8_t *key, uint32_t klen, const char *label, const uint8_t *data, uint32_t dlen,
             uint8_t *out, uint32_t out_len)
{
    uint8_t zero = 0, counter = 0, h[20];
    for (uint32_t done = 0; done < out_len; done += 20, counter++) {
        const uint8_t *parts[4] = { (const uint8_t *)label, &zero, data, &counter };
        uint32_t lens[4] = { (uint32_t)strlen(label), 1, dlen, 1 };
        hmac_sha1_v(key, klen, parts, lens, 4, h);
        uint32_t n = out_len - done < 20 ? out_len - done : 20;
        memcpy(out + done, h, n);
    }
}

static int memcmp_u(const uint8_t *a, const uint8_t *b, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

void wpa_ptk(const uint8_t pmk[32], const uint8_t aa[6], const uint8_t spa[6], const uint8_t anonce[32],
             const uint8_t snonce[32], uint8_t ptk[48])
{
    uint8_t data[76];
    if (memcmp_u(aa, spa, 6) < 0) { memcpy(data, aa, 6); memcpy(data + 6, spa, 6); }
    else { memcpy(data, spa, 6); memcpy(data + 6, aa, 6); }
    if (memcmp_u(anonce, snonce, 32) < 0) { memcpy(data + 12, anonce, 32); memcpy(data + 44, snonce, 32); }
    else { memcpy(data + 12, snonce, 32); memcpy(data + 44, anonce, 32); }
    wpa_prf(pmk, 32, "Pairwise key expansion", data, 76, ptk, 48);
}

/* ---------------------------------------------------------------- AES-128 */
static const uint8_t sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,
    0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,
    0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,
    0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,
    0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,
    0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16 };
static uint8_t inv_sbox[256];
static int inv_ready;

static uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); }
static uint8_t gmul(uint8_t a, uint8_t b)
{
    uint8_t r = 0;
    while (b) { if (b & 1) r ^= a; a = xtime(a); b >>= 1; }
    return r;
}

void aes128_init(struct aes128 *c, const uint8_t key[16])
{
    if (!inv_ready) { for (int i = 0; i < 256; i++) inv_sbox[sbox[i]] = (uint8_t)i; inv_ready = 1; }
    for (int i = 0; i < 4; i++)
        c->rk[i] = ((uint32_t)key[4 * i] << 24) | ((uint32_t)key[4 * i + 1] << 16) | ((uint32_t)key[4 * i + 2] << 8) | key[4 * i + 3];
    uint8_t rcon = 1;
    for (int i = 4; i < 44; i++) {
        uint32_t t = c->rk[i - 1];
        if (i % 4 == 0) {
            t = (t << 8) | (t >> 24);
            t = ((uint32_t)sbox[t >> 24] << 24) | ((uint32_t)sbox[(t >> 16) & 255] << 16) |
                ((uint32_t)sbox[(t >> 8) & 255] << 8) | sbox[t & 255];
            t ^= (uint32_t)rcon << 24;
            rcon = xtime(rcon);
        }
        c->rk[i] = c->rk[i - 4] ^ t;
    }
}

static void add_round_key(uint8_t s[16], const uint32_t *rk)
{
    for (int i = 0; i < 4; i++) {
        s[4 * i] ^= (uint8_t)(rk[i] >> 24); s[4 * i + 1] ^= (uint8_t)(rk[i] >> 16);
        s[4 * i + 2] ^= (uint8_t)(rk[i] >> 8); s[4 * i + 3] ^= (uint8_t)rk[i];
    }
}

void aes128_encrypt(const struct aes128 *c, const uint8_t in[16], uint8_t out[16])
{
    uint8_t s[16]; memcpy(s, in, 16);
    add_round_key(s, c->rk);
    for (int r = 1; r <= 10; r++) {
        for (int i = 0; i < 16; i++) s[i] = sbox[s[i]];
        uint8_t t[16];                                    /* shift rows (state is column-major: s[4*col+row]) */
        for (int col = 0; col < 4; col++) for (int row = 0; row < 4; row++) t[4 * col + row] = s[4 * ((col + row) % 4) + row];
        if (r != 10) {                                    /* mix columns */
            for (int col = 0; col < 4; col++) {
                uint8_t *p = t + 4 * col, a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
                p[0] = (uint8_t)(xtime(a0) ^ (xtime(a1) ^ a1) ^ a2 ^ a3);
                p[1] = (uint8_t)(a0 ^ xtime(a1) ^ (xtime(a2) ^ a2) ^ a3);
                p[2] = (uint8_t)(a0 ^ a1 ^ xtime(a2) ^ (xtime(a3) ^ a3));
                p[3] = (uint8_t)((xtime(a0) ^ a0) ^ a1 ^ a2 ^ xtime(a3));
            }
        }
        memcpy(s, t, 16);
        add_round_key(s, c->rk + 4 * r);
    }
    memcpy(out, s, 16);
}

void aes128_decrypt(const struct aes128 *c, const uint8_t in[16], uint8_t out[16])
{
    uint8_t s[16]; memcpy(s, in, 16);
    add_round_key(s, c->rk + 40);
    for (int r = 9; r >= 0; r--) {
        uint8_t t[16];                                    /* inverse shift rows */
        for (int col = 0; col < 4; col++) for (int row = 0; row < 4; row++) t[4 * ((col + row) % 4) + row] = s[4 * col + row];
        for (int i = 0; i < 16; i++) t[i] = inv_sbox[t[i]];
        add_round_key(t, c->rk + 4 * r);
        if (r != 0) {                                     /* inverse mix columns */
            for (int col = 0; col < 4; col++) {
                uint8_t *p = t + 4 * col, a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
                p[0] = (uint8_t)(gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9));
                p[1] = (uint8_t)(gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13));
                p[2] = (uint8_t)(gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11));
                p[3] = (uint8_t)(gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14));
            }
        }
        memcpy(s, t, 16);
    }
    memcpy(out, s, 16);
}

/* ------------------------------------------------------- AES key unwrap */
int aes_key_unwrap(const uint8_t kek[16], const uint8_t *in, uint32_t in_len, uint8_t *out)
{
    if (in_len < 24 || (in_len & 7)) return -1;
    uint32_t n = in_len / 8 - 1;
    struct aes128 c; aes128_init(&c, kek);
    uint8_t a[8]; memcpy(a, in, 8);
    memcpy(out, in + 8, n * 8);
    for (int j = 5; j >= 0; j--) {
        for (uint32_t i = n; i >= 1; i--) {
            uint32_t t = n * (uint32_t)j + i;
            uint8_t b[16];
            memcpy(b, a, 8);
            b[7] ^= (uint8_t)t; b[6] ^= (uint8_t)(t >> 8); b[5] ^= (uint8_t)(t >> 16); b[4] ^= (uint8_t)(t >> 24);
            memcpy(b + 8, out + (i - 1) * 8, 8);
            aes128_decrypt(&c, b, b);
            memcpy(a, b, 8);
            memcpy(out + (i - 1) * 8, b + 8, 8);
        }
    }
    for (int i = 0; i < 8; i++) if (a[i] != 0xA6) return -1;
    return 0;
}
