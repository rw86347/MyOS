/*
 * driver.c — a small coverage-guided fuzzer.
 *
 * Why not libFuzzer: Apple's clang ships the AddressSanitizer and
 * UndefinedBehaviorSanitizer runtimes but not libclang_rt.fuzzer, so
 * -fsanitize=fuzzer does not link on this machine. What it does ship is the
 * instrumentation, -fsanitize-coverage=trace-pc-guard, which calls a hook on
 * every edge. A usable fuzzer is that hook plus a mutation loop, which is
 * what this file is. Build with FUZZER=libfuzzer to use the real one instead
 * where it exists; the targets are plain LLVMFuzzerTestOneInput either way.
 *
 * How it works: keep a corpus of inputs. Pick one, mutate it, run it, and
 * see whether it reached an edge no earlier input did. If it did, keep it.
 * Repeat. Coverage is the only signal, which is enough to teach the fuzzer
 * things like "set the QR bit or the DNS parser is never entered" without
 * being told.
 *
 * The three things that make it find bugs rather than just run:
 *   ASan/UBSan   turn a silent overrun into an immediate abort
 *   a death hook save the input that killed us, so it can be replayed
 *   an alarm     a run that never finishes is a bug too (CVE-2020-24338 was
 *                a DNS name that pointed at itself)
 */

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "targets.h"

int  LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int  LLVMFuzzerInitialize(int *argc, char ***argv);

/* ---------------------------------------------------------------- coverage */

/*
 * One byte per edge. The instrumentation hands out a guard per edge and we
 * number them at startup; the hook is three instructions because it runs
 * tens of millions of times a second and anything else would dominate.
 */
#define COV_MAX (1u << 21)
static uint8_t  cov_seen[COV_MAX];
static uint32_t cov_total;      /* edges the instrumentation created */
static uint32_t cov_hit;        /* edges reached at least once */
static uint32_t cov_new;        /* edges first reached by the current run */

void __sanitizer_cov_trace_pc_guard_init(uint32_t *start, uint32_t *stop)
{
    static uint32_t next = 1;           /* 0 means "already counted" */
    for (uint32_t *p = start; p < stop; p++)
        if (next < COV_MAX)
            *p = next++;
    cov_total = next - 1;
}

void __sanitizer_cov_trace_pc_guard(uint32_t *guard)
{
    uint32_t i = *guard;
    if (i && !cov_seen[i]) {
        cov_seen[i] = 1;
        cov_new++;
        cov_hit++;
    }
}

/*
 * The death callback comes from the sanitizer runtime, so it only exists in
 * the sanitizer build. The coverage build (make fuzz-coverage) has no
 * runtime to provide it, and a weak reference is not enough on arm64 Mach-O,
 * so the reference is compiled out instead of linked away.
 */
#if defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define HAVE_SANITIZER 1
#  endif
#endif

#ifdef HAVE_SANITIZER
void __sanitizer_set_death_callback(void (*cb)(void));
#endif

/* ------------------------------------------------------------------ corpus */

struct input {
    uint8_t *d;
    size_t   n;
};

static struct input *corpus;
static size_t corpus_n, corpus_cap;

static void corpus_add(const uint8_t *d, size_t n)
{
    if (corpus_n == corpus_cap) {
        corpus_cap = corpus_cap ? corpus_cap * 2 : 64;
        corpus = realloc(corpus, corpus_cap * sizeof *corpus);
        if (!corpus) {
            perror("realloc");
            exit(1);
        }
    }
    uint8_t *copy = malloc(n ? n : 1);
    if (!copy) {
        perror("malloc");
        exit(1);
    }
    memcpy(copy, d, n);
    corpus[corpus_n].d = copy;
    corpus[corpus_n].n = n;
    corpus_n++;
}

/* ------------------------------------------------------------------- random
   xorshift64*, seeded from the command line: same seed, same run. */

static uint64_t rng_state = 0x2545F4914F6CDD1Dull;

static uint64_t rnd(void)
{
    uint64_t x = rng_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    rng_state = x;
    return x * 0x2545F4914F6CDD1Dull;
}

static uint32_t rnd_below(uint32_t n) { return n ? (uint32_t)(rnd() % n) : 0; }

/* ---------------------------------------------------------------- mutation */

/*
 * Values that sit on a boundary somewhere in a packet: zero and one, the
 * signed and unsigned edges of a byte, and the 16-bit edges that lengths and
 * ports live near. A length field set to 0xFFFF is how Ripple20 started.
 */
static const uint8_t interesting8[] = { 0, 1, 0x7F, 0x80, 0xFF, 0x20, 0x40 };
static const uint16_t interesting16[] = { 0, 1, 0x7F, 0x80, 0xFF, 0x100, 0x7FFF,
                                          0x8000, 0xFFFF, 20, 28, 53, 60, 1500 };

#define MAX_LEN 2048

static size_t mutate(uint8_t *b, size_t n, size_t cap)
{
    switch (rnd_below(11)) {
    case 0:                                     /* flip one bit */
        if (n)
            b[rnd_below((uint32_t)n)] ^= (uint8_t)(1u << rnd_below(8));
        break;
    case 1:                                     /* random byte */
        if (n)
            b[rnd_below((uint32_t)n)] = (uint8_t)rnd();
        break;
    case 2:                                     /* a boundary byte */
        if (n)
            b[rnd_below((uint32_t)n)] = interesting8[rnd_below(sizeof interesting8)];
        break;
    case 3:                                     /* a boundary 16-bit, big-endian:
                                                   every length on the wire is */
        if (n >= 2) {
            uint32_t o = rnd_below((uint32_t)n - 1);
            uint16_t v = interesting16[rnd_below(sizeof interesting16 / 2)];
            b[o] = (uint8_t)(v >> 8);
            b[o + 1] = (uint8_t)v;
        }
        break;
    case 4:                                     /* add or subtract, big-endian 16 */
        if (n >= 2) {
            uint32_t o = rnd_below((uint32_t)n - 1);
            uint16_t v = (uint16_t)((b[o] << 8) | b[o + 1]);
            v = (uint16_t)(v + (int)rnd_below(35) - 17);
            b[o] = (uint8_t)(v >> 8);
            b[o + 1] = (uint8_t)v;
        }
        break;
    case 5:                                     /* add or subtract, big-endian 32 */
        if (n >= 4) {
            uint32_t o = rnd_below((uint32_t)n - 3);
            uint32_t v = ((uint32_t)b[o] << 24) | ((uint32_t)b[o + 1] << 16) |
                         ((uint32_t)b[o + 2] << 8) | b[o + 3];
            v += rnd_below(35) - 17u;
            b[o] = (uint8_t)(v >> 24); b[o + 1] = (uint8_t)(v >> 16);
            b[o + 2] = (uint8_t)(v >> 8); b[o + 3] = (uint8_t)v;
        }
        break;
    case 6: {                                   /* insert a run of bytes */
        uint32_t add = 1 + rnd_below(16);
        if (n + add > cap)
            break;
        uint32_t o = rnd_below((uint32_t)n + 1);
        memmove(b + o + add, b + o, n - o);
        uint8_t fill = (uint8_t)rnd();
        for (uint32_t i = 0; i < add; i++)
            b[o + i] = (rnd() & 1) ? fill : (uint8_t)rnd();
        n += add;
        break;
    }
    case 7: {                                   /* erase a run */
        if (n < 2)
            break;
        uint32_t cut = 1 + rnd_below((uint32_t)n - 1);
        uint32_t o = rnd_below((uint32_t)n - cut + 1);
        memmove(b + o, b + o + cut, n - o - cut);
        n -= cut;
        break;
    }
    case 8: {                                   /* duplicate a chunk: how you
                                                   build a compression loop */
        if (!n)
            break;
        uint32_t len = 1 + rnd_below(n < 32 ? (uint32_t)n : 32);
        if (n + len > cap)
            break;
        uint32_t from = rnd_below((uint32_t)n - len + 1);
        uint32_t to = rnd_below((uint32_t)n + 1);
        memmove(b + to + len, b + to, n - to);
        memmove(b + to, b + (from >= to ? from + len : from), len);
        n += len;
        break;
    }
    case 9:                                     /* swap two bytes */
        if (n >= 2) {
            uint32_t a = rnd_below((uint32_t)n), c = rnd_below((uint32_t)n);
            uint8_t t = b[a];
            b[a] = b[c];
            b[c] = t;
        }
        break;
    case 10:                                    /* splice in another input */
        if (corpus_n) {
            struct input *o = &corpus[rnd_below((uint32_t)corpus_n)];
            if (o->n) {
                uint32_t take = 1 + rnd_below((uint32_t)(o->n < 64 ? o->n : 64));
                uint32_t at = rnd_below((uint32_t)n + 1);
                if (at + take <= cap) {
                    memcpy(b + at, o->d, take);
                    if (at + take > n)
                        n = at + take;
                }
            }
        }
        break;
    }
    return n;
}

/* --------------------------------------------------------- crash reporting */

static uint8_t cur[MAX_LEN];
static size_t  cur_n;
static const char *out_dir = "build/fuzz";
static const char *target_name = "?";
static unsigned long long iteration;

static void write_file(const char *dir, const char *tag,
                       const uint8_t *d, size_t n)
{
    char path[512];
    mkdir(dir, 0755);
    snprintf(path, sizeof path, "%s/%s-%s-%llu", dir, tag, target_name, iteration);
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "cannot write %s: %s\n", path, strerror(errno));
        return;
    }
    if (n)
        fwrite(d, 1, n, f);
    fclose(f);
    fprintf(stderr, "input saved to %s (%zu bytes)\n", path, n);
}

/* The sanitizers call this just before they kill the process, which is the
   only moment we still know which input did it. */
static void on_death(void)
{
    char dir[512];
    snprintf(dir, sizeof dir, "%s/crashes", out_dir);
    write_file(dir, "crash", cur, cur_n);
}

static void on_alarm(int sig)
{
    (void)sig;
    char dir[512];
    snprintf(dir, sizeof dir, "%s/crashes", out_dir);
    /* Not async-signal-safe in the strict sense, but we are about to die
       anyway and keeping the input matters more than purity. */
    write_file(dir, "hang", cur, cur_n);
    _exit(70);
}

/* ------------------------------------------------------------------- driver */

static const struct fuzz_target *pick(const char *name)
{
    unsigned n;
    const struct fuzz_target *t = fuzz_targets(&n);
    for (unsigned i = 0; i < n; i++)
        if (strcmp(t[i].name, name) == 0)
            return &t[i];
    return NULL;
}

static void list_targets(void)
{
    unsigned n;
    const struct fuzz_target *t = fuzz_targets(&n);
    fprintf(stderr, "targets:");
    for (unsigned i = 0; i < n; i++)
        fprintf(stderr, " %s", t[i].name);
    fprintf(stderr, "\n");
}

static const struct fuzz_target *active;

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > MAX_LEN)
        size = MAX_LEN;
    fuzz_run(active, data, size);
    if (fuzz_leaked()) {
        fprintf(stderr, "FINDING: packets are being leaked (skb_live high)\n");
        abort();
    }
    return 0;
}

static int load_file(const char *path, uint8_t *b, size_t cap, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    *n = fread(b, 1, cap, f);
    fclose(f);
    return 0;
}

static void load_dir(const char *path)
{
    char cmd[640];
    /* A directory listing without pulling in dirent handling: the corpus is
       a flat directory of files and this is a build tool, not the kernel. */
    snprintf(cmd, sizeof cmd, "ls -1 '%s' 2>/dev/null", path);
    FILE *p = popen(cmd, "r");
    if (!p)
        return;
    char name[256];
    while (fgets(name, sizeof name, p)) {
        name[strcspn(name, "\n")] = 0;
        if (!name[0])
            continue;
        char full[640];
        snprintf(full, sizeof full, "%s/%s", path, name);
        uint8_t b[MAX_LEN];
        size_t n;
        if (load_file(full, b, sizeof b, &n) == 0)
            corpus_add(b, n);
    }
    pclose(p);
}

static void usage(void)
{
    fprintf(stderr,
        "usage: fuzz_net TARGET [options]        fuzz\n"
        "       fuzz_net TARGET -replay FILE...  run given inputs once\n"
        "options: -runs=N -seed=N -corpus=DIR -out=DIR\n");
    list_targets();
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage();
        return 2;
    }
    target_name = argv[1];
    active = pick(target_name);
    if (!active) {
        fprintf(stderr, "no such target: %s\n", target_name);
        list_targets();
        return 2;
    }

    unsigned long long runs = 200000;
    const char *corpus_dir = NULL;
    int replay = 0, replay_first = 0;

    for (int i = 2; i < argc; i++) {
        if (strncmp(argv[i], "-runs=", 6) == 0)
            runs = strtoull(argv[i] + 6, NULL, 10);
        else if (strncmp(argv[i], "-seed=", 6) == 0)
            rng_state = strtoull(argv[i] + 6, NULL, 10) | 1;
        else if (strncmp(argv[i], "-corpus=", 8) == 0)
            corpus_dir = argv[i] + 8;
        else if (strncmp(argv[i], "-out=", 5) == 0)
            out_dir = argv[i] + 5;
        else if (strcmp(argv[i], "-replay") == 0) {
            replay = 1;
            replay_first = i + 1;
            break;
        } else {
            usage();
            return 2;
        }
    }

#ifdef HAVE_SANITIZER
    __sanitizer_set_death_callback(on_death);
#else
    (void)on_death;
#endif
    signal(SIGALRM, on_alarm);

    fuzz_init();

    if (replay) {
        for (int i = replay_first; i < argc; i++) {
            size_t n;
            if (load_file(argv[i], cur, sizeof cur, &n) < 0) {
                fprintf(stderr, "cannot read %s\n", argv[i]);
                return 2;
            }
            cur_n = n;
            alarm(30);
            cov_new = 0;
            LLVMFuzzerTestOneInput(cur, cur_n);
            printf("ok   %s (%zu bytes, %u edges)\n", argv[i], n, cov_hit);
        }
        alarm(0);
        printf("replay: %d input(s), %u of %u edges\n",
               argc - replay_first, cov_hit, cov_total);
        return 0;
    }

    if (corpus_dir)
        load_dir(corpus_dir);
    if (!corpus_n) {
        static const uint8_t empty[64] = { 0 };
        corpus_add(empty, sizeof empty);
    }

    /* Run the seeds first, so their coverage is counted as the baseline and
       the loop below only keeps inputs that add something. */
    size_t seeds = corpus_n;
    for (size_t i = 0; i < seeds; i++) {
        memcpy(cur, corpus[i].d, corpus[i].n);
        cur_n = corpus[i].n;
        alarm(30);
        LLVMFuzzerTestOneInput(cur, cur_n);
    }
    fprintf(stderr, "%s: %zu seed(s), %u of %u edges\n",
            target_name, seeds, cov_hit, cov_total);

    char corpus_out[512];
    snprintf(corpus_out, sizeof corpus_out, "%s/corpus-%s", out_dir, target_name);

    time_t t0 = time(NULL);
    unsigned long long found = 0;
    for (iteration = 1; iteration <= runs; iteration++) {
        struct input *parent = &corpus[rnd_below((uint32_t)corpus_n)];
        cur_n = parent->n <= MAX_LEN ? parent->n : MAX_LEN;
        memcpy(cur, parent->d, cur_n);
        unsigned rounds = 1 + rnd_below(4);
        for (unsigned r = 0; r < rounds; r++)
            cur_n = mutate(cur, cur_n, MAX_LEN);

        cov_new = 0;
        /* Re-arming each run resets the timer, so it only fires if one
           single run takes 30 seconds — an infinite loop in a parser. */
        alarm(30);
        LLVMFuzzerTestOneInput(cur, cur_n);

        if (cov_new) {
            corpus_add(cur, cur_n);
            write_file(corpus_out, "cov", cur, cur_n);
            found++;
        }
        if ((iteration % 50000) == 0) {
            double secs = difftime(time(NULL), t0);
            fprintf(stderr, "%s: %llu runs, %u/%u edges, corpus %zu, %.0f runs/s\n",
                    target_name, iteration, cov_hit, cov_total, corpus_n,
                    secs > 0 ? iteration / secs : 0.0);
        }
    }
    alarm(0);
    fprintf(stderr, "%s: done, %llu runs, %u of %u edges, %llu new input(s)\n",
            target_name, runs, cov_hit, cov_total, found);
    return 0;
}
