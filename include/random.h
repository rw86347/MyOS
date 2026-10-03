#ifndef MYOS_RANDOM_H
#define MYOS_RANDOM_H
#include <stdint.h>

void     random_init(void);                 /* collect boot entropy, key the CSPRNG */
void     random_add(const void *data, uint32_t len);    /* stir something in */
void     random_bytes(void *out, uint32_t len);
uint32_t random_u32(void);

/*
 * A keyed pseudorandom function over up to 16 bytes, with a secret chosen at
 * boot: the same input always gives the same answer, and an attacker who can
 * see any number of answers cannot work out the secret or predict another.
 * This is RFC 6528's F() for TCP sequence numbers. Cheap enough once per
 * connection, far too slow per packet — a counter keyed by this is the way to
 * do per-packet values (see ip_frame).
 */
uint32_t random_keyed(const void *data, uint32_t len);

/* The ChaCha20 block function, exposed only so a self-test can check it
   against the RFC 8439 vector. key: 8 words, nonce: 3 words, out: 16 words. */
void chacha20_block(const uint32_t key[8], uint32_t counter,
                    const uint32_t nonce[3], uint32_t out[16]);

#endif
