#include <string.h>
#include "zigbee_ccm.h"

typedef struct {
    void (*encrypt_block)(void *ctx, uint8_t block[16]);
    void *ctx;
} aes_t;

// CBC-MAC absorb: XORs `len` bytes into the running block `x`, encrypting each time it fills up.
static void mac_absorb(const aes_t *aes, uint8_t x[16], size_t *pos, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        x[(*pos)++] ^= data[i];
        if (*pos == 16) {
            aes->encrypt_block(aes->ctx, x);
            *pos = 0;
        }
    }
}

// Zero-pads the data absorbed so far to a whole block.
static void mac_pad(const aes_t *aes, uint8_t x[16], size_t *pos)
{
    if (*pos != 0) {
        aes->encrypt_block(aes->ctx, x);
        *pos = 0;
    }
}

// The unencrypted MIC T over B_0, the authenticated data (with its 2-byte length) and the plaintext.
static void ccm_mac(const aes_t *aes, const uint8_t *nonce, const uint8_t *auth, size_t auth_len, const uint8_t *plain,
                    size_t len, size_t tag_len, uint8_t x[16])
{
    size_t pos = 0;

    x[0] = (uint8_t)((auth_len ? 0x40 : 0) | (((tag_len - 2) / 2) << 3) | 1); // L - 1 = 1
    memcpy(&x[1], nonce, 13);
    x[14] = (uint8_t)(len >> 8);
    x[15] = (uint8_t)len;
    aes->encrypt_block(aes->ctx, x);
    if (auth_len) {
        const uint8_t auth_len_be[2] = {(uint8_t)(auth_len >> 8), (uint8_t)auth_len};

        mac_absorb(aes, x, &pos, auth_len_be, sizeof(auth_len_be));
        mac_absorb(aes, x, &pos, auth, auth_len);
        mac_pad(aes, x, &pos);
    }
    mac_absorb(aes, x, &pos, plain, len);
    mac_pad(aes, x, &pos);
}

// Counter block i encrypted: the keystream for payload block i (i >= 1) or the MIC mask (i = 0).
static void ccm_keystream(const aes_t *aes, const uint8_t *nonce, uint16_t i, uint8_t out[16])
{
    out[0] = 1; // L - 1
    memcpy(&out[1], nonce, 13);
    out[14] = (uint8_t)(i >> 8);
    out[15] = (uint8_t)i;
    aes->encrypt_block(aes->ctx, out);
}

static void ccm_ctr(const aes_t *aes, const uint8_t *nonce, const uint8_t *input, uint8_t *output, size_t len)
{
    uint8_t stream[16];

    for (size_t off = 0; off < len; off += 16) {
        ccm_keystream(aes, nonce, (uint16_t)(off / 16 + 1), stream);
        for (size_t j = 0; j < 16 && off + j < len; j++) {
            output[off + j] = input[off + j] ^ stream[j];
        }
    }
}

int zigbee_ccm_crypt(void (*encrypt_block)(void *ctx, uint8_t block[16]), void *ctx, bool encrypt,
                     const uint8_t nonce[13], const uint8_t *auth, size_t auth_len, const uint8_t *in, uint8_t *out,
                     size_t len, uint8_t *tag, size_t tag_len)
{
    if (encrypt_block == NULL || nonce == NULL || (auth == NULL && auth_len != 0) || (len != 0 && (in == NULL || out == NULL))) {
        return -1;
    }
    if (len > UINT16_MAX || auth_len >= 0xFF00 || tag_len > 16 || (tag_len != 0 && (tag_len < 4 || tag_len % 2 != 0 || tag == NULL))) {
        return -1;
    }

    const aes_t aes = {encrypt_block, ctx};
    uint8_t x[16];
    uint8_t mask[16];

    if (encrypt) { // the MIC is over the plaintext, which `out` may overwrite
        if (tag_len) {
            ccm_mac(&aes, nonce, auth, auth_len, in, len, tag_len, x);
        }
        ccm_ctr(&aes, nonce, in, out, len);
        if (tag_len) {
            ccm_keystream(&aes, nonce, 0, mask);
            for (size_t i = 0; i < tag_len; i++) {
                tag[i] = x[i] ^ mask[i];
            }
        }
        return 0;
    }

    ccm_ctr(&aes, nonce, in, out, len);
    if (tag_len == 0) {
        return 0;
    }
    uint8_t diff = 0;

    ccm_mac(&aes, nonce, auth, auth_len, out, len, tag_len, x);
    ccm_keystream(&aes, nonce, 0, mask);
    for (size_t i = 0; i < tag_len; i++) {
        diff |= (uint8_t)(tag[i] ^ x[i] ^ mask[i]);
    }
    if (diff) {
        memset(out, 0, len); // never hand out plaintext that did not authenticate
        return -1;
    }
    return 0;
}
