/*
 * random_test.c — boot-time self-tests for the CSPRNG (only in `make test`).
 *
 * The first test is the one that matters. A broken ChaCha20 still produces
 * output that looks perfectly random, so no statistical check would catch a
 * wrong rotation or a swapped word — only the published answer will. This is
 * the test vector from RFC 8439 §2.3.2, and everything in the stack that
 * depends on unpredictable numbers depends on it passing.
 */
#include <random.h>
#include <random_test.h>
#include <selftest.h>
#include <string.h>

static void test_chacha20_vector(void)
{
    /* RFC 8439 2.3.2: key 00,01,02...1f, counter 1, nonce 00:00:00:09 00:00:00:4a 00:00:00:00 */
    uint32_t key[8] = {
        0x03020100, 0x07060504, 0x0B0A0908, 0x0F0E0D0C,
        0x13121110, 0x17161514, 0x1B1A1918, 0x1F1E1D1C,
    };
    uint32_t nonce[3] = { 0x09000000, 0x4A000000, 0x00000000 };
    static const uint32_t want[16] = {
        0xE4E7F110, 0x15593BD1, 0x1FDD0F50, 0xC47120A3,
        0xC7F4D1C7, 0x0368C033, 0x9AAA2204, 0x4E6CD4C3,
        0x466482D2, 0x09AA9F07, 0x05D7C214, 0xA2028BD9,
        0xD19C12B5, 0xB94E16DE, 0xE883D0CB, 0x4E3C50A2,
    };
    uint32_t out[16];
    chacha20_block(key, 1, nonce, out);
    check(memcmp(out, want, sizeof want) == 0, "random: ChaCha20 matches RFC 8439");
}

static void test_bytes(void)
{
    /* Not a test of randomness — you cannot test that — but of the plumbing:
       successive calls must not repeat, and a short request must not leave
       the rest of the caller's buffer untouched or overrun it. */
    uint32_t a[8], b[8];
    random_bytes(a, sizeof a);
    random_bytes(b, sizeof b);
    check(memcmp(a, b, sizeof a) != 0, "random: successive blocks differ");

    int same = 0;
    for (int i = 0; i < 8; i++)
        for (int j = i + 1; j < 8; j++)
            if (a[i] == a[j])
                same++;
    check(same == 0, "random: words within a block differ");

    uint8_t buf[16];
    memset(buf, 0xAA, sizeof buf);
    random_bytes(buf, 5);
    check(buf[5] == 0xAA && buf[15] == 0xAA, "random: a short request writes only 5 bytes");

    /* The spare buffer is 32 bytes, so this crosses a refill boundary and
       must not repeat the 32 bytes it already handed out. */
    uint8_t big[96];
    random_bytes(big, sizeof big);
    check(memcmp(big, big + 32, 32) != 0 && memcmp(big + 32, big + 64, 32) != 0,
          "random: output spanning several blocks does not repeat");
}

static void test_keyed(void)
{
    uint8_t tuple[12] = { 10, 0, 2, 15, 0xC0, 0x00, 10, 0, 2, 2, 0, 80 };
    uint32_t first = random_keyed(tuple, sizeof tuple);
    check(random_keyed(tuple, sizeof tuple) == first, "random: keyed is the same answer twice");

    tuple[11] = 81;                 /* one bit of the port */
    check(random_keyed(tuple, sizeof tuple) != first, "random: keyed follows its input");

    /* Different lengths of the same bytes must not collide, or two
       four-tuples that share a prefix would share a sequence number. */
    check(random_keyed(tuple, 4) != random_keyed(tuple, 8),
          "random: keyed depends on the whole input");
}

void random_selftest(void)
{
    test_chacha20_vector();
    test_bytes();
    test_keyed();
}
