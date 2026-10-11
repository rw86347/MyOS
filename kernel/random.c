/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * random.c — the kernel's random number generator.
 *
 * Why this file exists: four separate network security bugs were all the same
 * bug, "we have no randomness". A TCP sequence number, an ephemeral port, a
 * DNS transaction id and an IP ID were each a counter an off-path attacker
 * could predict after seeing one value, which is most of what the RFC 6528,
 * RFC 6056 and Kaminsky attacks need. One generator fixes all four.
 *
 * Two halves, and they are different problems:
 *
 *   1. Getting some bits nobody can guess, once, at boot. Hard, because this
 *      hardware gives us almost nothing. See stir_boot below.
 *   2. Turning those bits into an endless stream. Easy and well understood:
 *      ChaCha20, the same cipher Linux's /dev/urandom uses.
 *
 * ChaCha20 suits this machine. It is 32-bit adds, xors and rotates and
 * nothing else — no tables to miss in the cache, no multiplies, and no SSE,
 * which matters because the kernel is built -mgeneral-regs-only and may not
 * touch the FPU registers at all.
 *
 * Nothing here is on a hot path. Every caller wants a few bytes once per
 * connection or once per lookup. The one per-packet user, the IP ID, uses a
 * counter keyed by random_keyed rather than calling the generator per packet.
 */
#include <cycles.h>
#include <interrupts.h>
#include <io.h>
#include <random.h>
#include <rtc.h>
#include <string.h>

/* --- ChaCha20 ------------------------------------------------------------ */

#define ROTL(v, n) ((v) << (n) | (v) >> (32 - (n)))
#define QR(a, b, c, d)                                                  \
    do {                                                                \
        a += b; d ^= a; d = ROTL(d, 16);                                \
        c += d; b ^= c; b = ROTL(b, 12);                                \
        a += b; d ^= a; d = ROTL(d, 8);                                 \
        c += d; b ^= c; b = ROTL(b, 7);                                 \
    } while (0)

void chacha20_block(const uint32_t key[8], uint32_t counter,
                    const uint32_t nonce[3], uint32_t out[16])
{
    /* "expand 32-byte k" — the constant from RFC 8439, as four words. */
    uint32_t s[16] = {
        0x61707865, 0x3320646E, 0x79622D32, 0x6B206574,
        key[0], key[1], key[2], key[3],
        key[4], key[5], key[6], key[7],
        counter, nonce[0], nonce[1], nonce[2],
    };
    for (int i = 0; i < 16; i++)
        out[i] = s[i];
    for (int i = 0; i < 10; i++) {          /* 10 double rounds = 20 rounds */
        QR(out[0], out[4], out[8],  out[12]);   /* columns */
        QR(out[1], out[5], out[9],  out[13]);
        QR(out[2], out[6], out[10], out[14]);
        QR(out[3], out[7], out[11], out[15]);
        QR(out[0], out[5], out[10], out[15]);   /* diagonals */
        QR(out[1], out[6], out[11], out[12]);
        QR(out[2], out[7], out[8],  out[13]);
        QR(out[3], out[4], out[9],  out[14]);
    }
    for (int i = 0; i < 16; i++)
        out[i] += s[i];
}

/* --- boot entropy -------------------------------------------------------- */

/*
 * The target machines have no RDRAND and no RDSEED: Intel added RDRAND in Ivy
 * Bridge, years after the Pentium 4 and the Pentium M. There is no hardware
 * random number generator to ask, so the entropy has to be measured.
 *
 * What is measurable is how long things take. A port read to 0x80 is an ISA
 * bus cycle, about a microsecond, and the cycle counter says how long it
 * really took. That number is not constant on real hardware: DRAM refresh,
 * bus contention, cache and TLB state, SMIs the operating system never sees,
 * and the CPU's own clock domain crossing all move the low bits. Linux does
 * the same thing in try_to_generate_entropy() when it has nothing better.
 *
 * We keep the *second* difference — the change in the interval rather than the
 * interval — because the interval itself has a large predictable part and only
 * its variation is unpredictable. Each sample contributes perhaps one or two
 * bits, which is why there are 512 of them, and they are folded into a pool
 * that is rotated so that later samples cannot cancel earlier ones.
 *
 * Be honest about the limit: under QEMU with -icount the cycle counter is
 * deterministic, so on the emulator this produces the same pool every boot.
 * The numbers are only real on real hardware, which is the machine that
 * matters. docs/security.md says so out loud.
 */
static void stir_boot(uint32_t pool[8])
{
    uint64_t prev = cycles_now(), interval = 0;
    for (int i = 0; i < 512; i++) {
        (void)inb(0x80);                /* the POST port: unused, and slow */
        uint64_t now = cycles_now();
        uint64_t d = now - prev;
        prev = now;
        uint32_t jitter = (uint32_t)(d - interval);
        interval = d;
        pool[i & 7] = ROTL(pool[i & 7], 7) ^ jitter;
    }
}

/* --- the generator ------------------------------------------------------- */

/*
 * "Fast key erasure": every block of output also produces the next key, and
 * the old key is overwritten. So an attacker who reads kernel memory now
 * cannot work backwards to the bytes we handed out before, which a plain
 * stream cipher keyed once at boot would let them do.
 *
 * The cost is one ChaCha20 block per 32 bytes of output rather than per 64.
 * That is the right trade here: no caller asks for bulk random data on a path
 * where it matters, and getrandom() is a syscall, not a loop in the kernel.
 */
static uint32_t key[8];
static uint32_t counter;
static uint8_t  spare[32];          /* output not yet handed out */
static uint32_t spare_n;
static int      ready;

static uint32_t keyed_secret[8];    /* for random_keyed; never leaves here */

static void refill(void)
{
    uint32_t out[16];
    /* The nonce carries the cycle counter, so even if the pool at boot was
       poor, every block mixes in whatever timing variation has happened
       since. It cannot make the output worse: ChaCha20 is secure for an
       attacker-chosen nonce, which this is not. */
    uint64_t t = cycles_now();
    uint32_t nonce[3] = { (uint32_t)t, (uint32_t)(t >> 32), counter };
    chacha20_block(key, counter++, nonce, out);
    memcpy(key, out, sizeof key);               /* the old key is gone */
    memcpy(spare, out + 8, sizeof spare);
    spare_n = sizeof spare;
    memset(out, 0, sizeof out);
}

void random_init(void)
{
    uint32_t pool[8];
    memset(pool, 0, sizeof pool);
    stir_boot(pool);
    /* Two things that differ between machines and between boots but are not
       secret: they add uniqueness, not strength, which is worth having
       because two machines booting the same image must not agree. */
    uint64_t t = cycles_now();
    pool[0] ^= rtc_read_epoch();
    pool[1] ^= (uint32_t)t;
    pool[2] ^= (uint32_t)(t >> 32);
    /* xor rather than assign: anything random_add stirred in before we got
       here is kept rather than thrown away. */
    for (int i = 0; i < 8; i++)
        key[i] ^= pool[i];
    memset(pool, 0, sizeof pool);
    ready = 1;
    random_bytes(keyed_secret, sizeof keyed_secret);
}

/*
 * Stir something in without claiming it is secret. Used for the network card's
 * MAC address and anything else discovered after random_init: hashing it into
 * the key can only help, because ChaCha20's key schedule does not care where
 * the bits came from.
 */
void random_add(const void *data, uint32_t len)
{
    const uint8_t *p = data;
    uint32_t flags = irq_save();
    for (uint32_t i = 0; i < len; i++)
        ((uint8_t *)key)[i & 31] ^= p[i];
    if (ready)
        refill();                   /* fold it through the cipher at once */
    irq_restore(flags);
}

/*
 * The key, the counter and the spare buffer are one shared object, and two
 * callers interleaving in it would be bad in a specific way: both would read
 * the same spare_n and be handed *the same bytes*, or one would read bytes the
 * other had already wiped and get zeros. Either one puts two connections on
 * the same sequence number or the same port — the bug this file exists to
 * prevent, arriving by a different road.
 *
 * It is tempting to argue the lock is unnecessary, because syscalls run with
 * interrupts off (`int 0x80` is an interrupt gate, sysenter clears IF) and the
 * network thread holds them off on purpose. That argument is wrong, and worth
 * recording as wrong: a syscall is not atomic, because copy_to_user and
 * copy_from_user can sleep. A fault on a page of an mmap'd file sends
 * mm_fault to the page cache, which waits for the disk, and another thread
 * runs. Interrupts being off only closes the window where no copy happens.
 * Nothing in this file copies to user memory, so the irq_save below is what
 * actually makes each pass atomic rather than merely looking like it does.
 *
 * It is around the state and not around the loop. A caller may ask for a
 * megabyte, and holding interrupts off for all of it would wreck the
 * worst-case latency this kernel is built for; one pass is a single ChaCha20
 * block and a 32-byte copy.
 */
void random_bytes(void *out, uint32_t len)
{
    uint8_t *o = out;
    while (len) {
        uint8_t chunk[sizeof spare];
        uint32_t flags = irq_save();
        if (!spare_n)
            refill();
        uint32_t n = len < spare_n ? len : spare_n;
        memcpy(chunk, spare + sizeof spare - spare_n, n);
        memset(spare + sizeof spare - spare_n, 0, n);    /* handed out once */
        spare_n -= n;
        irq_restore(flags);
        memcpy(o, chunk, n);
        memset(chunk, 0, n);        /* and not left on the kernel stack */
        o += n;
        len -= n;
    }
}

uint32_t random_u32(void)
{
    uint32_t v;
    random_bytes(&v, sizeof v);
    return v;
}

uint32_t random_keyed(const void *data, uint32_t len)
{
    uint32_t in[4] = { 0, 0, 0, 0 };
    if (len > sizeof in)
        len = sizeof in;
    memcpy(in, data, len);
    uint32_t out[16];
    /* The input is the nonce and the block counter; the key is the secret.
       Different inputs therefore give unrelated outputs, and the secret stays
       hidden however many outputs an attacker collects. */
    chacha20_block(keyed_secret, in[3], in, out);
    return out[0];
}
