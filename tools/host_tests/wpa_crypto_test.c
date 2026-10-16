/*
 * Host test for sdk/src/wpa_crypto.c against published vectors.
 *   clang -DHOST_TEST -I sdk/include -o build/wpa_crypto_test tools/host_tests/wpa_crypto_test.c sdk/src/wpa_crypto.c
 */
#include <stdio.h>
#include <string.h>
#include "wpa_crypto.h"

static int fails;

static void check(const char *what, const uint8_t *got, const char *want_hex, int n)
{
    char hex[200];
    for (int i = 0; i < n; i++) sprintf(hex + 2 * i, "%02x", got[i]);
    int ok = strcmp(hex, want_hex) == 0;
    printf("%-28s %s\n", what, ok ? "OK" : "FAIL");
    if (!ok) { printf("   got  %s\n   want %s\n", hex, want_hex); fails++; }
}

int main(void)
{
    uint8_t h[20], k[64], out[48];

    sha1((const uint8_t *)"abc", 3, h);
    check("SHA-1(abc)", h, "a9993e364706816aba3e25717850c26c9cd0d89d", 20);

    memset(k, 0x0b, 20);
    hmac_sha1(k, 20, (const uint8_t *)"Hi There", 8, h);
    check("HMAC-SHA1 RFC2202 #1", h, "b617318655057264e28bc0b6fb378c8ef146be00", 20);

    /* IEEE 802.11 test vector: passphrase "password", SSID "IEEE" */
    wpa_pmk("password", (const uint8_t *)"IEEE", 4, out);
    check("PMK(password, IEEE)", out, "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e", 32);

    /* FIPS-197 C.1 */
    static const uint8_t key[16] = { 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15 };
    static const uint8_t pt[16] = { 0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff };
    struct aes128 c; aes128_init(&c, key);
    uint8_t ct[16], back[16];
    aes128_encrypt(&c, pt, ct);
    check("AES-128 encrypt FIPS-197", ct, "69c4e0d86a7b0430d8cdb78070b4c55a", 16);
    aes128_decrypt(&c, ct, back);
    check("AES-128 decrypt FIPS-197", back, "00112233445566778899aabbccddeeff", 16);

    /* RFC 3394 4.1: 128-bit KEK, 128-bit key */
    static const uint8_t wrapped[24] = { 0x1F,0xA6,0x8B,0x0A,0x81,0x12,0xB4,0x47,0xAE,0xF3,0x4B,0xD8,0xFB,0x5A,0x7B,0x82,
                                         0x9D,0x3E,0x86,0x23,0x71,0xD2,0xCF,0xE5 };
    int rc = aes_key_unwrap(key, wrapped, 24, out);
    printf("%-28s %s\n", "AES unwrap integrity", rc == 0 ? "OK" : "FAIL"); if (rc) fails++;
    check("AES unwrap RFC3394 4.1", out, "00112233445566778899aabbccddeeff", 16);

    /* PTK: known-answer from the PMK above with fixed addresses/nonces, computed by Python (see test) */
    static const uint8_t aa[6] = { 0x02,0x00,0x00,0x00,0x00,0x01 }, spa[6] = { 0x02,0x00,0x00,0x00,0x00,0x02 };
    uint8_t an[32], sn[32];
    for (int i = 0; i < 32; i++) { an[i] = (uint8_t)i; sn[i] = (uint8_t)(0xFF - i); }
    uint8_t pmk[32];
    wpa_pmk("password", (const uint8_t *)"IEEE", 4, pmk);
    wpa_ptk(pmk, aa, spa, an, sn, out);
    char hex[100]; for (int i = 0; i < 48; i++) sprintf(hex + 2 * i, "%02x", out[i]);
    printf("PTK (compare with python): %s\n", hex);

    printf("%s\n", fails ? "SOME TESTS FAILED" : "ALL OK");
    return fails ? 1 : 0;
}
