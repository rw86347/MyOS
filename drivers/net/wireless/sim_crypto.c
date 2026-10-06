/*
 * sim_crypto.c — the cryptography a WPA2-Personal access point needs, for
 * the simulated one (sim.c), in `make test` kernels only:
 *
 *   SHA-1 (FIPS 180-4), HMAC-SHA1 (RFC 2104)
 *   PBKDF2-HMAC-SHA1 (RFC 8018): passphrase + SSID -> the 256-bit PMK
 *   the 802.11 PRF (IEEE 802.11-2020 12.7.1.2): PMK -> the session keys
 *   AES-128 encryption (FIPS 197) and AES key wrap (RFC 3394): the
 *     group key, sent to the station inside handshake message 3
 *
 * Written plainly, for checking rather than speed (the self-test runs the
 * standards' own test vectors). The station's side is wpa_supplicant's.
 */
#include <linux/kernel.h>
#include <linux/types.h>
#include <string.h>

/* --- SHA-1 ----------------------------------------------------------------------- */

struct sha1 {
    u32 h[5];
    u64 len;                            /* bytes so far */
    u8 buf[64];
    u32 used;
};

static u32 rol(u32 x, int n) { return x << n | x >> (32 - n); }

static void sha1_block(u32 h[5], const u8 *p)
{
    u32 w[80];
    for (int i = 0; i < 16; i++)
        w[i] = (u32)p[4 * i] << 24 | (u32)p[4 * i + 1] << 16 | (u32)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++)
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        u32 f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5a827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
        else { f = b ^ c ^ d; k = 0xca62c1d6; }
        u32 t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

static void sha1_init(struct sha1 *s)
{
    static const u32 iv[5] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0 };
    memcpy(s->h, iv, sizeof iv);
    s->len = 0;
    s->used = 0;
}

static void sha1_update(struct sha1 *s, const void *data, size_t n)
{
    const u8 *p = data;
    s->len += n;
    while (n) {
        u32 take = 64 - s->used < n ? 64 - s->used : (u32)n;
        memcpy(s->buf + s->used, p, take);
        s->used += take;
        p += take;
        n -= take;
        if (s->used == 64) {
            sha1_block(s->h, s->buf);
            s->used = 0;
        }
    }
}

static void sha1_final(struct sha1 *s, u8 out[20])
{
    u64 bits = s->len * 8;
    u8 pad = 0x80, zero = 0;
    sha1_update(s, &pad, 1);
    while (s->used != 56)
        sha1_update(s, &zero, 1);
    u8 lenb[8];
    for (int i = 0; i < 8; i++)
        lenb[i] = (u8)(bits >> (56 - 8 * i));
    sha1_update(s, lenb, 8);
    for (int i = 0; i < 5; i++) {
        out[4 * i] = (u8)(s->h[i] >> 24);
        out[4 * i + 1] = (u8)(s->h[i] >> 16);
        out[4 * i + 2] = (u8)(s->h[i] >> 8);
        out[4 * i + 3] = (u8)s->h[i];
    }
}

/* HMAC-SHA1 of the n parts (key at most 64 bytes, as all ours are). */
void sim_hmac_sha1(const u8 *key, size_t klen, const u8 *const *part, const size_t *plen, int n,
                   u8 out[20])
{
    u8 k[64] = { 0 }, pad[64], inner[20];
    memcpy(k, key, klen > 64 ? 64 : klen);
    struct sha1 s;
    for (int i = 0; i < 64; i++)
        pad[i] = k[i] ^ 0x36;
    sha1_init(&s);
    sha1_update(&s, pad, 64);
    for (int i = 0; i < n; i++)
        sha1_update(&s, part[i], plen[i]);
    sha1_final(&s, inner);
    for (int i = 0; i < 64; i++)
        pad[i] = k[i] ^ 0x5c;
    sha1_init(&s);
    sha1_update(&s, pad, 64);
    sha1_update(&s, inner, 20);
    sha1_final(&s, out);
}

static void hmac1(const u8 *key, size_t klen, const u8 *msg, size_t mlen, u8 out[20])
{
    sim_hmac_sha1(key, klen, &msg, &mlen, 1, out);
}

/* The WPA2-Personal PMK: PBKDF2-HMAC-SHA1(passphrase, SSID, 4096, 32). */
void sim_pbkdf2_sha1(const char *pass, const u8 *ssid, size_t ssid_len, u8 pmk[32])
{
    size_t plen = strlen(pass);
    for (u32 block = 1; block <= 2; block++) {
        u8 salt[32 + 4], u[20], t[20];
        memcpy(salt, ssid, ssid_len);
        salt[ssid_len] = (u8)(block >> 24);
        salt[ssid_len + 1] = (u8)(block >> 16);
        salt[ssid_len + 2] = (u8)(block >> 8);
        salt[ssid_len + 3] = (u8)block;
        hmac1((const u8 *)pass, plen, salt, ssid_len + 4, u);
        memcpy(t, u, 20);
        for (int i = 1; i < 4096; i++) {
            hmac1((const u8 *)pass, plen, u, 20, u);
            for (int j = 0; j < 20; j++)
                t[j] ^= u[j];
        }
        memcpy(pmk + (block - 1) * 20, t, block == 1 ? 20 : 12);
    }
}

/* PRF-n: HMAC-SHA1(K, label || 0 || data || i), i = 0, 1, ..., to n bytes. */
void sim_prf_sha1(const u8 *key, size_t klen, const char *label, const u8 *data, size_t dlen,
                  u8 *out, size_t n)
{
    u8 zero = 0, i = 0, d[20];
    size_t llen = strlen(label);
    for (size_t at = 0; at < n; at += 20, i++) {
        const u8 *part[4] = { (const u8 *)label, &zero, data, &i };
        const size_t plen[4] = { llen, 1, dlen, 1 };
        sim_hmac_sha1(key, klen, part, plen, 4, d);
        memcpy(out + at, d, n - at < 20 ? n - at : 20);
    }
}

/* --- AES-128 ------------------------------------------------------------------------- */

static const u8 sbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};

static u8 xtime(u8 x) { return (u8)(x << 1 ^ (x & 0x80 ? 0x1b : 0)); }

/* The 11 round keys from the 16-byte key. */
static void aes_expand(const u8 key[16], u8 rk[176])
{
    u8 rcon = 1;
    memcpy(rk, key, 16);
    for (int i = 16; i < 176; i += 4) {
        u8 t[4] = { rk[i - 4], rk[i - 3], rk[i - 2], rk[i - 1] };
        if (i % 16 == 0) {
            u8 r = t[0];
            t[0] = (u8)(sbox[t[1]] ^ rcon);
            t[1] = sbox[t[2]];
            t[2] = sbox[t[3]];
            t[3] = sbox[r];
            rcon = xtime(rcon);
        }
        for (int j = 0; j < 4; j++)
            rk[i + j] = rk[i - 16 + j] ^ t[j];
    }
}

static void aes_encrypt(const u8 rk[176], const u8 in[16], u8 out[16])
{
    u8 s[16];
    for (int i = 0; i < 16; i++)
        s[i] = in[i] ^ rk[i];
    for (int round = 1; round <= 10; round++) {
        u8 t[16];
        for (int i = 0; i < 16; i++)    /* SubBytes + ShiftRows (column-major state) */
            t[i] = sbox[s[(i + 4 * (i % 4)) % 16]];
        if (round < 10)
            for (int c = 0; c < 4; c++) {       /* MixColumns */
                u8 *col = t + 4 * c, a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                u8 all = a0 ^ a1 ^ a2 ^ a3;
                col[0] ^= all ^ xtime(a0 ^ a1);
                col[1] ^= all ^ xtime(a1 ^ a2);
                col[2] ^= all ^ xtime(a2 ^ a3);
                col[3] ^= all ^ xtime(a3 ^ a0);
            }
        for (int i = 0; i < 16; i++)
            s[i] = t[i] ^ rk[16 * round + i];
    }
    memcpy(out, s, 16);
}

/* AES key wrap (RFC 3394): n 8-byte blocks of plain -> n + 1 blocks. */
void sim_aes_wrap(const u8 kek[16], const u8 *plain, size_t n, u8 *out)
{
    u8 rk[176], a[8], b[16];
    aes_expand(kek, rk);
    memset(a, 0xa6, 8);
    memcpy(out + 8, plain, n * 8);
    for (int j = 0; j <= 5; j++)
        for (size_t i = 1; i <= n; i++) {
            memcpy(b, a, 8);
            memcpy(b + 8, out + 8 * i, 8);
            aes_encrypt(rk, b, b);
            u64 t = (u64)n * (u64)j + i;
            memcpy(a, b, 8);
            for (int k = 0; k < 8; k++)
                a[7 - k] ^= (u8)(t >> (8 * k));
            memcpy(out + 8 * i, b + 8, 8);
        }
    memcpy(out, a, 8);
}

/* (For the self-test: one AES block.) */
void sim_aes_block(const u8 key[16], const u8 in[16], u8 out[16])
{
    u8 rk[176];
    aes_expand(key, rk);
    aes_encrypt(rk, in, out);
}
