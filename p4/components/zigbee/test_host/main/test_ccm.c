#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "unity.h"
#include "zigbee_ccm.h"
#include "aes_ref.h"
#include "ccm_boundary.h"

static const uint8_t KEY[16] = {0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7,
                                0xC8, 0xC9, 0xCA, 0xCB, 0xCC, 0xCD, 0xCE, 0xCF
                               };

static void aes_block(void *ctx, uint8_t block[16])
{
    aes_ref_encrypt(ctx, block);
}

static void with_key(void (*body)(aes_ref_t *))
{
    aes_ref_t aes;

    aes_ref_setkey(&aes, KEY);
    body(&aes);
}

static int crypt(aes_ref_t *aes, bool encrypt, const uint8_t nonce[13], const uint8_t *auth, size_t auth_len,
                 const uint8_t *in, uint8_t *out, size_t len, uint8_t *tag, size_t tag_len)
{
    return zigbee_ccm_crypt(aes_block, aes, encrypt, nonce, auth, auth_len, in, out, len, tag, tag_len);
}

/* Unity rejects comparing 0 bytes; an empty payload or no MIC has nothing to compare. */
static void expect_bytes(const uint8_t *expected, const uint8_t *actual, size_t n, const char *what)
{
    if (n > 0) {
        TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(expected, actual, n, what);
    }
}

/* One known-answer case: ciphertext || MIC is `expected`, of `len + tag_len` bytes. */
typedef struct {
    const uint8_t *nonce;
    const uint8_t *auth;
    size_t auth_len;
    const uint8_t *plain;
    size_t len;
    size_t tag_len;
    const uint8_t *expected;
} kat_t;

static const kat_t *g_kat;

static void kat_encrypt(aes_ref_t *aes)
{
    uint8_t out[64] = {0};
    uint8_t tag[16] = {0};

    TEST_ASSERT_EQUAL(0, crypt(aes, true, g_kat->nonce, g_kat->auth, g_kat->auth_len, g_kat->plain, out, g_kat->len,
                               tag, g_kat->tag_len));
    expect_bytes(g_kat->expected, out, g_kat->len, "ciphertext");
    expect_bytes(g_kat->expected + g_kat->len, tag, g_kat->tag_len, "MIC");
}

static void kat_decrypt(aes_ref_t *aes)
{
    uint8_t out[64] = {0};
    uint8_t tag[16];

    memcpy(tag, g_kat->expected + g_kat->len, g_kat->tag_len);
    TEST_ASSERT_EQUAL(0, crypt(aes, false, g_kat->nonce, g_kat->auth, g_kat->auth_len, g_kat->expected, out,
                               g_kat->len, tag, g_kat->tag_len));
    expect_bytes(g_kat->plain, out, g_kat->len, "plaintext");
}

static void check_encrypt(const kat_t *kat)
{
    g_kat = kat;
    with_key(kat_encrypt);
}

static void check_decrypt(const kat_t *kat)
{
    g_kat = kat;
    with_key(kat_decrypt);
}

/* ---- RFC 3610 section 8, packet vectors 1-3 (M = 8, L = 2), key C0..CF ---- */

static const uint8_t RFC_AAD[8] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
static const uint8_t RFC_PLAIN[25] = {0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14,
                                      0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20
                                     };
static const uint8_t RFC1_NONCE[13] = {0x00, 0x00, 0x00, 0x03, 0x02, 0x01, 0x00, 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5};
static const uint8_t RFC2_NONCE[13] = {0x00, 0x00, 0x00, 0x04, 0x03, 0x02, 0x01, 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5};
static const uint8_t RFC3_NONCE[13] = {0x00, 0x00, 0x00, 0x05, 0x04, 0x03, 0x02, 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5};
static const uint8_t RFC1_OUT[31] = {0x58, 0x8C, 0x97, 0x9A, 0x61, 0xC6, 0x63, 0xD2, 0xF0, 0x66, 0xD0,
                                     0xC2, 0xC0, 0xF9, 0x89, 0x80, 0x6D, 0x5F, 0x6B, 0x61, 0xDA, 0xC3,
                                     0x84, 0x17, 0xE8, 0xD1, 0x2C, 0xFD, 0xF9, 0x26, 0xE0
                                    };
static const uint8_t RFC2_OUT[32] = {0x72, 0xC9, 0x1A, 0x36, 0xE1, 0x35, 0xF8, 0xCF, 0x29, 0x1C, 0xA8,
                                     0x94, 0x08, 0x5C, 0x87, 0xE3, 0xCC, 0x15, 0xC4, 0x39, 0xC9, 0xE4,
                                     0x3A, 0x3B, 0xA0, 0x91, 0xD5, 0x6E, 0x10, 0x40, 0x09, 0x16
                                    };
static const uint8_t RFC3_OUT[33] = {0x51, 0xB1, 0xE5, 0xF4, 0x4A, 0x19, 0x7D, 0x1D, 0xA4, 0x6B, 0x0F,
                                     0x8E, 0x2D, 0x28, 0x2A, 0xE8, 0x71, 0xE8, 0x38, 0xBB, 0x64, 0xDA,
                                     0x85, 0x96, 0x57, 0x4A, 0xDA, 0xA7, 0x6F, 0xBD, 0x9F, 0xB0, 0xC5
                                    };

static const kat_t RFC1 = {RFC1_NONCE, RFC_AAD, 8, RFC_PLAIN, 23, 8, RFC1_OUT};
static const kat_t RFC2 = {RFC2_NONCE, RFC_AAD, 8, RFC_PLAIN, 24, 8, RFC2_OUT};
static const kat_t RFC3 = {RFC3_NONCE, RFC_AAD, 8, RFC_PLAIN, 25, 8, RFC3_OUT};

TEST_CASE("RFC 3610 packet vector 1: encrypt writes the expected ciphertext and MIC", "[zigbee]")
{
    check_encrypt(&RFC1);
}

TEST_CASE("RFC 3610 packet vector 2: encrypt writes the expected ciphertext and MIC", "[zigbee]")
{
    check_encrypt(&RFC2);
}

TEST_CASE("RFC 3610 packet vector 3: encrypt writes the expected ciphertext and MIC", "[zigbee]")
{
    check_encrypt(&RFC3);
}

TEST_CASE("RFC 3610 packet vector 1: decrypt with the right MIC writes the plaintext and returns 0", "[zigbee]")
{
    check_decrypt(&RFC1);
}

TEST_CASE("RFC 3610 packet vector 2: decrypt with the right MIC writes the plaintext and returns 0", "[zigbee]")
{
    check_decrypt(&RFC2);
}

TEST_CASE("RFC 3610 packet vector 3: decrypt with the right MIC writes the plaintext and returns 0", "[zigbee]")
{
    check_decrypt(&RFC3);
}

/* ---- IEEE 802.15.4-2006 annex C.2, key C0..CF, source ACDE480000000001, frame counter 5 ----
 * Annex C.2 has no MIC-32 case: C.2.1 is a beacon with MIC-64 (authentication only), C.2.2 a data frame with
 * ENC (encryption only, MIC length 0), C.2.3 a MAC command with ENC-MIC-64. */

static const uint8_t C21_NONCE[13] = {0xAC, 0xDE, 0x48, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x05, 0x02};
static const uint8_t C21_HEADER[26] = {0x08, 0xD0, 0x84, 0x21, 0x43, 0x01, 0x00, 0x00, 0x00, 0x00, 0x48, 0xDE, 0xAC,
                                       0x02, 0x05, 0x00, 0x00, 0x00, 0x55, 0xCF, 0x00, 0x00, 0x51, 0x52, 0x53, 0x54
                                      };
static const uint8_t C21_MIC[8] = {0x22, 0x3B, 0xC1, 0xEC, 0x84, 0x1A, 0xB5, 0x53};

static const uint8_t C22_NONCE[13] = {0xAC, 0xDE, 0x48, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x05, 0x04};
static const uint8_t C22_HEADER[26] = {0x69, 0xDC, 0x84, 0x21, 0x43, 0x02, 0x00, 0x00, 0x00, 0x00, 0x48, 0xDE, 0xAC,
                                       0x01, 0x00, 0x00, 0x00, 0x00, 0x48, 0xDE, 0xAC, 0x04, 0x05, 0x00, 0x00, 0x00
                                      };
static const uint8_t C22_PLAIN[4] = {0x61, 0x62, 0x63, 0x64};
static const uint8_t C22_CIPHER[4] = {0xD4, 0x3E, 0x02, 0x2B};

static const uint8_t C23_NONCE[13] = {0xAC, 0xDE, 0x48, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x05, 0x06};
static const uint8_t C23_HEADER[29] = {0x2B, 0xDC, 0x84, 0x21, 0x43, 0x02, 0x00, 0x00, 0x00, 0x00, 0x48, 0xDE, 0xAC, 0xFF, 0xFF,
                                       0x01, 0x00, 0x00, 0x00, 0x00, 0x48, 0xDE, 0xAC, 0x06, 0x05, 0x00, 0x00, 0x00, 0x01
                                      };
static const uint8_t C23_PLAIN[1] = {0xCE};
static const uint8_t C23_OUT[9] = {0xD8, 0x4F, 0xDE, 0x52, 0x90, 0x61, 0xF9, 0xC6, 0xF1};

static const kat_t C21 = {C21_NONCE, C21_HEADER, sizeof(C21_HEADER), NULL, 0, 8, C21_MIC};
static const kat_t C22 = {C22_NONCE, C22_HEADER, sizeof(C22_HEADER), C22_PLAIN, 4, 0, C22_CIPHER};
static const kat_t C23 = {C23_NONCE, C23_HEADER, sizeof(C23_HEADER), C23_PLAIN, 1, 8, C23_OUT};

TEST_CASE("802.15.4 C.2.1 beacon, zero-length payload: encrypt writes the MIC only", "[zigbee]")
{
    check_encrypt(&C21);
}

TEST_CASE("802.15.4 C.2.1 beacon, zero-length payload: decrypt with the right MIC returns 0", "[zigbee]")
{
    check_decrypt(&C21);
}

TEST_CASE("802.15.4 C.2.2 data frame, MIC length 0: encrypt writes the ciphertext and no MIC", "[zigbee]")
{
    check_encrypt(&C22);
}

TEST_CASE("802.15.4 C.2.2 data frame, MIC length 0: decrypt writes the plaintext and returns 0", "[zigbee]")
{
    check_decrypt(&C22);
}

TEST_CASE("802.15.4 C.2.3 command frame: encrypt writes the expected ciphertext and MIC", "[zigbee]")
{
    check_encrypt(&C23);
}

TEST_CASE("802.15.4 C.2.3 command frame: decrypt with the right MIC writes the plaintext and returns 0", "[zigbee]")
{
    check_decrypt(&C23);
}

/* ---- boundary sizes and every MIC length (ccm_boundary.h) ---- */

static void boundary_pattern(uint8_t *auth, size_t auth_len, uint8_t *plain, size_t len)
{
    for (size_t i = 0; i < auth_len; i++) {
        auth[i] = (uint8_t)(i * 7 + 3);
    }
    for (size_t i = 0; i < len; i++) {
        plain[i] = (uint8_t)(i * 13 + 1);
    }
}

static void boundary_encrypt_all(aes_ref_t *aes)
{
    static const uint8_t nonce[13] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
    static uint8_t auth[512];
    uint8_t plain[64];

    for (size_t i = 0; i < sizeof(ccm_boundary_cases) / sizeof(ccm_boundary_cases[0]); i++) {
        const ccm_boundary_case_t *c = &ccm_boundary_cases[i];
        uint8_t out[64] = {0};
        uint8_t tag[16] = {0};

        boundary_pattern(auth, c->auth_len, plain, c->len);
        TEST_ASSERT_EQUAL_MESSAGE(0, crypt(aes, true, nonce, auth, c->auth_len, plain, out, c->len, tag, c->tag_len),
                                  "encrypt");
        expect_bytes(c->expected, out, c->len, "ciphertext");
        expect_bytes(c->expected + c->len, tag, c->tag_len, "MIC");
    }
}

static void boundary_decrypt_all(aes_ref_t *aes)
{
    static const uint8_t nonce[13] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
    static uint8_t auth[512];
    uint8_t plain[64];

    for (size_t i = 0; i < sizeof(ccm_boundary_cases) / sizeof(ccm_boundary_cases[0]); i++) {
        const ccm_boundary_case_t *c = &ccm_boundary_cases[i];
        uint8_t out[64] = {0};
        uint8_t tag[16];

        boundary_pattern(auth, c->auth_len, plain, c->len);
        memcpy(tag, c->expected + c->len, c->tag_len);
        TEST_ASSERT_EQUAL_MESSAGE(0, crypt(aes, false, nonce, auth, c->auth_len, c->expected, out, c->len, tag, c->tag_len),
                                  "decrypt");
        expect_bytes(plain, out, c->len, "plaintext");
    }
}

TEST_CASE("boundary sizes and MIC lengths 4 to 16: encrypt writes the expected ciphertext and MIC", "[zigbee]")
{
    with_key(boundary_encrypt_all);
}

TEST_CASE("boundary sizes and MIC lengths 4 to 16: decrypt writes the plaintext and returns 0", "[zigbee]")
{
    with_key(boundary_decrypt_all);
}

/* ---- failures and aliasing ---- */

static void wrong_mic_flipped_tag(aes_ref_t *aes)
{
    uint8_t tag[8];
    uint8_t out[23];

    memcpy(tag, RFC1_OUT + 23, 8);
    tag[7] ^= 0x01;
    memset(out, 0xEE, sizeof(out));
    TEST_ASSERT_NOT_EQUAL(0, crypt(aes, false, RFC1_NONCE, RFC_AAD, 8, RFC1_OUT, out, 23, tag, 8));
}

TEST_CASE("decrypt with a wrong MIC: returns nonzero", "[zigbee]")
{
    with_key(wrong_mic_flipped_tag);
}

static void wrong_input_variants(aes_ref_t *aes)
{
    uint8_t cipher[23];
    uint8_t aad[8];
    uint8_t nonce[13];
    uint8_t out[23];

    memcpy(cipher, RFC1_OUT, 23);
    cipher[10] ^= 0x80;
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, crypt(aes, false, RFC1_NONCE, RFC_AAD, 8, cipher, out, 23, (uint8_t *)RFC1_OUT + 23, 8),
                                  "ciphertext");
    memcpy(aad, RFC_AAD, 8);
    aad[0] ^= 0x01;
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, crypt(aes, false, RFC1_NONCE, aad, 8, RFC1_OUT, out, 23, (uint8_t *)RFC1_OUT + 23, 8),
                                  "authenticated data");
    memcpy(nonce, RFC1_NONCE, 13);
    nonce[12] ^= 0x01;
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, crypt(aes, false, nonce, RFC_AAD, 8, RFC1_OUT, out, 23, (uint8_t *)RFC1_OUT + 23, 8),
                                  "nonce");
}

TEST_CASE("decrypt with a changed ciphertext, authenticated data or nonce: returns nonzero", "[zigbee]")
{
    with_key(wrong_input_variants);
}

static void wrong_mic_clears_output(aes_ref_t *aes)
{
    uint8_t tag[8];
    uint8_t out[23];
    uint8_t zeros[23] = {0};

    memcpy(tag, RFC1_OUT + 23, 8);
    tag[0] ^= 0xFF;
    memset(out, 0xEE, sizeof(out));
    TEST_ASSERT_NOT_EQUAL(0, crypt(aes, false, RFC1_NONCE, RFC_AAD, 8, RFC1_OUT, out, 23, tag, 8));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(zeros, out, 23);
}

TEST_CASE("decrypt with a wrong MIC: the output holds no plaintext", "[zigbee]")
{
    with_key(wrong_mic_clears_output);
}

static void in_place_both(aes_ref_t *aes)
{
    uint8_t buf[23];
    uint8_t tag[8];

    memcpy(buf, RFC_PLAIN, 23);
    TEST_ASSERT_EQUAL(0, crypt(aes, true, RFC1_NONCE, RFC_AAD, 8, buf, buf, 23, tag, 8));
    TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(RFC1_OUT, buf, 23, "ciphertext");
    TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(RFC1_OUT + 23, tag, 8, "MIC");
    TEST_ASSERT_EQUAL(0, crypt(aes, false, RFC1_NONCE, RFC_AAD, 8, buf, buf, 23, tag, 8));
    TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(RFC_PLAIN, buf, 23, "plaintext");
}

TEST_CASE("in and out are the same buffer: encrypt and decrypt give the same results", "[zigbee]")
{
    with_key(in_place_both);
}

static void mic_length_zero_ignores_tag(aes_ref_t *aes)
{
    uint8_t out[4];

    TEST_ASSERT_EQUAL(0, crypt(aes, true, C22_NONCE, C22_HEADER, 26, C22_PLAIN, out, 4, NULL, 0));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(C22_CIPHER, out, 4);
    TEST_ASSERT_EQUAL(0, crypt(aes, false, C22_NONCE, C22_HEADER, 26, C22_CIPHER, out, 4, NULL, 0));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(C22_PLAIN, out, 4);
}

TEST_CASE("MIC length 0 with no tag buffer: encrypt and decrypt work and touch no tag", "[zigbee]")
{
    with_key(mic_length_zero_ignores_tag);
}

static void invalid_arguments(aes_ref_t *aes)
{
    static const size_t bad_tag_lens[] = {1, 2, 3, 5, 7, 15, 17, 18, 32};
    static uint8_t big[65536 + 1];
    uint8_t out[16];
    uint8_t tag[32];

    for (size_t i = 0; i < sizeof(bad_tag_lens) / sizeof(bad_tag_lens[0]); i++) {
        TEST_ASSERT_NOT_EQUAL_MESSAGE(
            0, crypt(aes, true, RFC1_NONCE, RFC_AAD, 8, RFC_PLAIN, out, 16, tag, bad_tag_lens[i]), "bad MIC length");
    }
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, crypt(aes, true, RFC1_NONCE, RFC_AAD, 8, big, big, 65536, tag, 8), "payload 65536");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, crypt(aes, true, RFC1_NONCE, big, 0xFF00, RFC_PLAIN, out, 16, tag, 8), "aad 0xFF00");
    TEST_ASSERT_EQUAL_MESSAGE(0, crypt(aes, true, RFC1_NONCE, big, 0xFEFF, RFC_PLAIN, out, 16, tag, 8), "aad 0xFEFF");
    TEST_ASSERT_EQUAL_MESSAGE(0, crypt(aes, true, RFC1_NONCE, RFC_AAD, 8, big, big, 65535, tag, 8), "payload 65535");
}

TEST_CASE("invalid MIC length or sizes: returns nonzero, and the largest valid sizes work", "[zigbee]")
{
    with_key(invalid_arguments);
}

static void invalid_leaves_output(aes_ref_t *aes)
{
    uint8_t out[16];
    uint8_t tag[16];

    memset(out, 0x5A, sizeof(out));
    TEST_ASSERT_NOT_EQUAL(0, crypt(aes, true, RFC1_NONCE, RFC_AAD, 8, RFC_PLAIN, out, 16, tag, 5));
    for (size_t i = 0; i < sizeof(out); i++) {
        TEST_ASSERT_EQUAL_HEX8(0x5A, out[i]);
    }
}

TEST_CASE("invalid MIC length: out is not written", "[zigbee]")
{
    with_key(invalid_leaves_output);
}
