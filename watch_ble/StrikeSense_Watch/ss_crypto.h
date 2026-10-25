// =====================================================
// StrikeSense LoRa encryption (rod <-> watch)
//
// AES-128 (the standard, checked against the official test values):
// messages are encrypted (CTR) and signed (CMAC), so a watch only reads
// its own rod, and nobody can read or fake its messages.
// The SAME file is in the rod and the watch sketch folders. C and C++.
// =====================================================

#ifndef SS_CRYPTO_H
#define SS_CRYPTO_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
  uint8_t rk[176];          // the expanded key
} ss_aes_t;

void ss_aes_init(ss_aes_t *a, const uint8_t key[16]);
void ss_aes_block(const ss_aes_t *a, const uint8_t in[16], uint8_t out[16]);

// encrypt or decrypt (the same) with a 16-byte starting block (nonce)
void ss_aes_ctr(const ss_aes_t *a, const uint8_t nonce[16], uint8_t *data, size_t len);

// 16-byte signature of a message (AES-CMAC, RFC 4493)
void ss_aes_cmac(const ss_aes_t *a, const uint8_t *msg, size_t len, uint8_t mac[16]);

#ifdef __cplusplus
}
#endif

#endif
