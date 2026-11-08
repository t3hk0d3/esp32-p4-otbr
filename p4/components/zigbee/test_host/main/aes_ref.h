#pragma once

/*
 * A small AES-128 block encryption (FIPS-197) for the CCM* tests. The host build has no AES engine and mbedTLS 4 no
 * longer offers the legacy block API, and CCM* only needs the forward cipher. Test-only: not constant time. The
 * RFC 3610 and IEEE 802.15.4 vectors in test_ccm.c fail unless this is a correct AES.
 */

#include <stdint.h>
#include <string.h>

typedef struct {
    uint8_t round_keys[176];
} aes_ref_t;

static uint8_t aes_ref_xtime(uint8_t x)
{
    return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1B : 0));
}

static uint8_t aes_ref_sbox(uint8_t x)
{
    // The S-box is the multiplicative inverse in GF(2^8) followed by an affine map.
    uint8_t inv = 0;

    if (x != 0) {
        for (unsigned candidate = 1; candidate < 256; candidate++) {
            uint8_t product = 0;
            uint8_t a = (uint8_t)candidate;
            uint8_t b = x;

            while (b) {
                if (b & 1) {
                    product ^= a;
                }
                a = aes_ref_xtime(a);
                b >>= 1;
            }
            if (product == 1) {
                inv = (uint8_t)candidate;
                break;
            }
        }
    }
    uint8_t s = inv;

    for (int i = 1; i <= 4; i++) {
        s ^= (uint8_t)((inv << i) | (inv >> (8 - i)));
    }
    return s ^ 0x63;
}

static void aes_ref_setkey(aes_ref_t *aes, const uint8_t key[16])
{
    uint8_t rcon = 1;

    memcpy(aes->round_keys, key, 16);
    for (int i = 16; i < 176; i += 4) {
        uint8_t t[4];

        memcpy(t, &aes->round_keys[i - 4], 4);
        if (i % 16 == 0) {
            uint8_t first = t[0];

            t[0] = (uint8_t)(aes_ref_sbox(t[1]) ^ rcon);
            t[1] = aes_ref_sbox(t[2]);
            t[2] = aes_ref_sbox(t[3]);
            t[3] = aes_ref_sbox(first);
            rcon = aes_ref_xtime(rcon);
        }
        for (int j = 0; j < 4; j++) {
            aes->round_keys[i + j] = aes->round_keys[i - 16 + j] ^ t[j];
        }
    }
}

static void aes_ref_encrypt(const aes_ref_t *aes, uint8_t s[16])
{
    for (int round = 0; round <= 10; round++) {
        uint8_t t[16];

        if (round > 0) {
            for (int i = 0; i < 16; i++) {
                s[i] = aes_ref_sbox(s[i]);
            }
            // ShiftRows: column-major state, row r rotates left by r.
            for (int c = 0; c < 4; c++) {
                for (int r = 0; r < 4; r++) {
                    t[c * 4 + r] = s[((c + r) % 4) * 4 + r];
                }
            }
            memcpy(s, t, 16);
        }
        if (round > 0 && round < 10) {
            for (int c = 0; c < 4; c++) {
                uint8_t *col = &s[c * 4];
                uint8_t all = col[0] ^ col[1] ^ col[2] ^ col[3];
                uint8_t first = col[0];

                col[0] ^= all ^ aes_ref_xtime(col[0] ^ col[1]);
                col[1] ^= all ^ aes_ref_xtime(col[1] ^ col[2]);
                col[2] ^= all ^ aes_ref_xtime(col[2] ^ col[3]);
                col[3] ^= all ^ aes_ref_xtime(col[3] ^ first);
            }
        }
        for (int i = 0; i < 16; i++) {
            s[i] ^= aes->round_keys[round * 16 + i];
        }
    }
}
