#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * CCM* (IEEE 802.15.4-2006 annex B; RFC 3610 with a 13-byte nonce, so a 2-byte length field) over a caller-provided
 * AES-128 block encryption. The MIC is `tag_len` bytes: 0 (encryption only) or an even 4..16.
 *
 * @param encrypt_block Encrypts one 16-byte block in place with the key held in `ctx`.
 * @param ctx           Passed through to `encrypt_block`.
 * @param encrypt       true: write ciphertext to `out` and the MIC to `tag`; false: write plaintext to `out` and
 *                      check the MIC in `tag`.
 * @param nonce         13 bytes.
 * @param auth          Authenticated, not encrypted data (may be NULL when `auth_len` is 0).
 * @param in            `len` bytes of plaintext (encrypt) or ciphertext (decrypt).
 * @param out           `len` bytes. May be the same buffer as `in`; partially overlapping buffers are not supported.
 * @param tag           `tag_len` bytes, written (encrypt) or checked (decrypt). May be NULL when `tag_len` is 0.
 * @return 0 on success; nonzero on invalid arguments (`len` over 65535, `auth_len` over 65279, a bad `tag_len`) or on
 *         a MIC mismatch. After a failed decrypt `out` holds zeros, never plaintext that did not authenticate.
 */
int zigbee_ccm_crypt(void (*encrypt_block)(void *ctx, uint8_t block[16]), void *ctx, bool encrypt,
                     const uint8_t nonce[13], const uint8_t *auth, size_t auth_len, const uint8_t *in, uint8_t *out,
                     size_t len, uint8_t *tag, size_t tag_len);

#ifdef __cplusplus
}
#endif
