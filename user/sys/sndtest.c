/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * sndtest.c — the sound card from a program's side, with raw ALSA ioctls
 * (the ones alsa-lib makes), for either CPU. In `make test-sound` QEMU has
 * an HD Audio controller and codec whose speaker writes to a WAV file; this
 * plays a 440 Hz tone two ways — write() of frames (WRITEI), then straight
 * into the mapped buffer (mmap + SYNC_PTR + poll) — and the Makefile looks
 * for 440 Hz in the file. Test builds only.
 */
#include <stddef.h>
#include <stdint.h>
#if defined(__x86_64__)
#define ARCH_X86_64 1
#endif
#include "../../include/sound/asound.h"

#if defined(__x86_64__)
enum { S_read = 0, S_write = 1, S_open = 2, S_close = 3, S_poll = 7, S_mmap = 9,
       S_ioctl = 16, S_exit = 60 };
static long sc(long n, long a, long b, long c, long d, long e, long f)
{
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret)
                      : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                      : "rcx", "r11", "memory");
    return ret;
}
__asm__(".globl _start\n_start:\n  xor %rbp, %rbp\n  and $-16, %rsp\n  call cmain\n  hlt\n");
#define MMAP(a, l, p, f, fd, off) sc(S_mmap, (long)(a), (long)(l), p, f, fd, off)
#else
enum { S_read = 3, S_write = 4, S_open = 5, S_close = 6, S_poll = 168, S_mmap2 = 192,
       S_ioctl = 54, S_exit = 1 };
static long sc(long n, long a, long b, long c, long d, long e, long f)
{
    long ret;
    __asm__ volatile ("pushl %7\n\tpush %%ebp\n\tmov 4(%%esp), %%ebp\n\t"
                      "int $0x80\n\tpop %%ebp\n\tadd $4, %%esp"
                      : "=a"(ret)
                      : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e), "g"(f)
                      : "memory");
    return ret;
}
__asm__(".globl _start\n_start:\n  xor %ebp, %ebp\n  and $-16, %esp\n  call cmain\n  hlt\n");
#define MMAP(a, l, p, f, fd, off) sc(S_mmap2, (long)(a), (long)(l), p, f, fd, (off) / 4096)
#endif

#define O_RDWR      2
#define O_NONBLOCK  04000
#define PROT_RW     3
#define MAP_SHARED  1
#define POLLIN      1
#define POLLOUT     4

static long ioc(int fd, uint32_t cmd, void *arg) { return sc(S_ioctl, fd, (long)cmd, (long)arg, 0, 0, 0); }
static size_t slen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
static void out(const char *s) { sc(S_write, 1, (long)s, (long)slen(s), 0, 0, 0); }
static void outn(long v)
{
    char b[24], *p = b + sizeof b;
    int neg = v < 0;
    unsigned long u = neg ? -(unsigned long)v : (unsigned long)v;
    *--p = 0;
    do { *--p = (char)('0' + u % 10); u /= 10; } while (u);
    if (neg) *--p = '-';
    out(p);
}
static void zero(void *p, size_t n) { unsigned char *q = p; while (n--) *q++ = 0; }
static int same(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return *a == *b; }

static int failed;
static void check(int ok, const char *name, long detail)
{
    if (ok) { out("ok "); out(name); out("\n"); return; }
    failed++;
    out("FAIL "); out(name); out(" ("); outn(detail); out(")\n");
}

/* --- a tone ------------------------------------------------------------------- */

#define RATE    48000
#define PERIOD  1024
#define PERIODS 4

/* sin by a rotating recurrence: y[n] = 2 cos(w) y[n-1] - y[n-2]; cos(w)
   from its series (w is small), so no maths library is needed. */
static double y1v, y2v, k2;
static void tone_init(void)
{
    double w = 2 * 3.14159265358979 * 440 / RATE, w2 = w * w;
    k2 = 2 * (1 - w2 / 2 + w2 * w2 / 24 - w2 * w2 * w2 / 720);
    y1v = 0;
    y2v = -(w - w * w2 / 6 + w * w2 * w2 / 120);    /* sin(-w) */
}
static void tone(int16_t *buf, int frames)
{
    for (int i = 0; i < frames; i++) {
        double y = k2 * y1v - y2v;
        y2v = y1v;
        y1v = y;
        int16_t s = (int16_t)(y * 12000);
        buf[2 * i] = buf[2 * i + 1] = s;
    }
}

/* --- hw_params: everything open, then one value each ---------------------------- */

static void any(struct snd_pcm_hw_params *p)
{
    zero(p, sizeof *p);
    for (int k = 0; k < 3; k++)
        for (int j = 0; j < 8; j++)
            p->masks[k].bits[j] = ~0u;
    for (int k = 0; k < 12; k++)
        p->intervals[k].max = ~0u;
    p->rmask = ~0u;
}
static void set_mask(struct snd_pcm_hw_params *p, int n, unsigned int v)
{
    zero(&p->masks[n], sizeof p->masks[n]);
    p->masks[n].bits[v / 32] = 1u << (v % 32);
}
static void set_itv(struct snd_pcm_hw_params *p, int n, unsigned int v)
{
    struct snd_interval *i = &p->intervals[n - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
    i->min = i->max = v;
    i->openmin = i->openmax = 0;
    i->integer = 1;
}
static struct snd_interval *itv(struct snd_pcm_hw_params *p, int n)
{
    return &p->intervals[n - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
}

static int setup(int fd, unsigned int access, int show)
{
    struct snd_pcm_hw_params p;
    any(&p);
    long r = ioc(fd, SNDRV_PCM_IOCTL_HW_REFINE, &p);
    check(r == 0, "HW_REFINE", r);
    if (show) {
        out("sndtest: rates "); outn(itv(&p, SNDRV_PCM_HW_PARAM_RATE)->min); out("-");
        outn(itv(&p, SNDRV_PCM_HW_PARAM_RATE)->max); out(" Hz, periods ");
        outn(itv(&p, SNDRV_PCM_HW_PARAM_PERIODS)->min); out("-");
        outn(itv(&p, SNDRV_PCM_HW_PARAM_PERIODS)->max); out(", buffer up to ");
        outn(itv(&p, SNDRV_PCM_HW_PARAM_BUFFER_BYTES)->max); out(" bytes\n");
    }
    set_mask(&p, SNDRV_PCM_HW_PARAM_ACCESS, access);
    set_mask(&p, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
    set_itv(&p, SNDRV_PCM_HW_PARAM_CHANNELS, 2);
    set_itv(&p, SNDRV_PCM_HW_PARAM_RATE, RATE);
    set_itv(&p, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, PERIOD);
    set_itv(&p, SNDRV_PCM_HW_PARAM_PERIODS, PERIODS);
    p.rmask = ~0u;
    r = ioc(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &p);
    check(r == 0 && p.rate_num == RATE && itv(&p, SNDRV_PCM_HW_PARAM_BUFFER_SIZE)->min ==
          PERIOD * PERIODS, "HW_PARAMS 48 kHz, 4 x 1024 frames", r);

    struct snd_pcm_sw_params s;
    zero(&s, sizeof s);
    s.period_step = 1;
    s.avail_min = PERIOD;
    s.start_threshold = PERIOD * PERIODS;
    s.stop_threshold = PERIOD * PERIODS;
    s.boundary = PERIOD * PERIODS;
    s.proto = SNDRV_PCM_VERSION;
    r = ioc(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &s);
    check(r == 0 && s.boundary > PERIOD * PERIODS, "SW_PARAMS", r);
    r = ioc(fd, SNDRV_PCM_IOCTL_PREPARE, NULL);
    check(r == 0, "PREPARE", r);
    return (int)r;
}

static int16_t chunk[PERIOD * 2];

static void play_write(void)
{
    int fd = (int)sc(S_open, (long)"/dev/snd/pcmC0D0p", O_RDWR, 0, 0, 0, 0);
    check(fd >= 0, "open pcmC0D0p", fd);
    if (fd < 0)
        return;
    struct snd_pcm_info info;
    zero(&info, sizeof info);
    long r = ioc(fd, SNDRV_PCM_IOCTL_INFO, &info);
    check(r == 0 && info.stream == SNDRV_PCM_STREAM_PLAYBACK, "PCM INFO", r);
    out("sndtest: pcm \""); out((char *)info.name); out("\"\n");
    if (setup(fd, SNDRV_PCM_ACCESS_RW_INTERLEAVED, 1))
        return;
    long frames = 0;
    for (int k = 0; k < RATE / 2 / PERIOD; k++) {
        tone(chunk, PERIOD);
        struct snd_xferi x = { .buf = chunk, .frames = PERIOD };
        r = ioc(fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &x);
        if (r < 0 || x.result != PERIOD)
            break;
        frames += x.result;
    }
    check(frames == RATE / 2 / PERIOD * PERIOD, "WRITEI half a second", r);
    r = ioc(fd, SNDRV_PCM_IOCTL_DRAIN, NULL);
    check(r == 0, "DRAIN", r);
    struct snd_pcm_status st;
    ioc(fd, SNDRV_PCM_IOCTL_STATUS, &st);
    check(st.state == SNDRV_PCM_STATE_SETUP, "drained: state SETUP", st.state);
    sc(S_close, fd, 0, 0, 0, 0, 0);
}

static void play_mmap(void)
{
    int fd = (int)sc(S_open, (long)"/dev/snd/pcmC0D0p", O_RDWR, 0, 0, 0, 0);
    if (fd < 0 || setup(fd, SNDRV_PCM_ACCESS_MMAP_INTERLEAVED, 0))
        return;
    long bytes = PERIOD * PERIODS * 4;
    int16_t *map = (int16_t *)MMAP(0, bytes, PROT_RW, MAP_SHARED, fd, 0);
    check((unsigned long)map < (unsigned long)-4096, "mmap the buffer", (long)map);
    long status_map = MMAP(0, 4096, 1, MAP_SHARED, fd, 0x80000000L);
    check(status_map < 0, "no status page (alsa-lib uses SYNC_PTR)", status_map);
    if ((unsigned long)map >= (unsigned long)-4096)
        return;
    struct snd_pcm_sync_ptr sp;
    unsigned long appl = 0, total = RATE / 2 / PERIOD * PERIOD;
    int started = 0, waits = 0;
    long r = 0;
    while (appl < total) {
        zero(&sp, sizeof sp);
        sp.flags = SNDRV_PCM_SYNC_PTR_HWSYNC | SNDRV_PCM_SYNC_PTR_APPL | SNDRV_PCM_SYNC_PTR_AVAIL_MIN;
        if ((r = ioc(fd, SNDRV_PCM_IOCTL_SYNC_PTR, &sp)) < 0)
            break;
        unsigned long avail = sp.s.status.hw_ptr + PERIOD * PERIODS - appl;
        if (avail >= PERIOD) {
            tone(map + (appl % (PERIOD * PERIODS)) * 2, PERIOD);
            appl += PERIOD;
            zero(&sp, sizeof sp);
            sp.flags = SNDRV_PCM_SYNC_PTR_AVAIL_MIN;  /* we say appl_ptr */
            sp.c.control.appl_ptr = appl;
            if ((r = ioc(fd, SNDRV_PCM_IOCTL_SYNC_PTR, &sp)) < 0)
                break;
            if (!started && appl >= PERIOD * PERIODS) {
                r = ioc(fd, SNDRV_PCM_IOCTL_START, NULL);
                check(r == 0, "START (mmap)", r);
                started = 1;
            }
            continue;
        }
        struct { int fd; short events, revents; } pfd = { fd, POLLOUT, 0 };
        r = sc(S_poll, (long)&pfd, 1, 2000, 0, 0, 0);
        waits++;
        if (r <= 0)
            break;
    }
    check(appl == total && waits > 0, "mmap + SYNC_PTR + poll half a second", r);
    r = ioc(fd, SNDRV_PCM_IOCTL_DRAIN, NULL);
    check(r == 0, "DRAIN (mmap)", r);
    sc(S_close, fd, 0, 0, 0, 0, 0);
}

static void mixer(void)
{
    int fd = (int)sc(S_open, (long)"/dev/snd/controlC0", O_RDWR, 0, 0, 0, 0);
    check(fd >= 0, "open controlC0", fd);
    if (fd < 0)
        return;
    struct snd_ctl_card_info ci;
    long r = ioc(fd, SNDRV_CTL_IOCTL_CARD_INFO, &ci);
    check(r == 0 && same((char *)ci.driver, "HDA-Intel"), "CARD_INFO: driver HDA-Intel", r);
    out("sndtest: card \""); out((char *)ci.longname); out("\", mixer \"");
    out((char *)ci.mixername); out("\"\n");
    static struct snd_ctl_elem_id ids[32];
    struct snd_ctl_elem_list l = { .space = 32, .pids = ids };
    r = ioc(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &l);
    check(r == 0 && l.count > 0, "ELEM_LIST", r);
    int master = 0;
    for (unsigned int k = 0; k < l.used; k++) {
        out("sndtest: control "); out((char *)ids[k].name); out("\n");
        if (same((char *)ids[k].name, "Master Playback Volume"))
            master = (int)ids[k].numid;
    }
    if (master) {
        struct snd_ctl_elem_info info;
        zero(&info, sizeof info);
        info.id.numid = (unsigned int)master;
        r = ioc(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info);
        static struct snd_ctl_elem_value v;
        zero(&v, sizeof v);
        v.id.numid = (unsigned int)master;
        long r2 = ioc(fd, SNDRV_CTL_IOCTL_ELEM_READ, &v);
        check(r == 0 && r2 == 0 && info.value.integer.max > 0, "Master volume readable", r);
        out("sndtest: Master "); outn(v.value.integer[0]); out(" of ");
        outn(info.value.integer.max); out("\n");
        struct { struct snd_ctl_tlv h; unsigned int v[4]; } tlv = { { (unsigned int)master, 16 }, { 0 } };
        r = ioc(fd, SNDRV_CTL_IOCTL_TLV_READ, &tlv);
        check(r == 0 && tlv.v[0] == SNDRV_CTL_TLVT_DB_SCALE, "Master dB scale (TLV)", r);
    }
    sc(S_close, fd, 0, 0, 0, 0, 0);
}

static void record(void)
{
    int fd = (int)sc(S_open, (long)"/dev/snd/pcmC0D0c", O_RDWR | O_NONBLOCK, 0, 0, 0, 0);
    if (fd < 0) {
        out("sndtest: no capture device\n");
        return;
    }
    if (setup(fd, SNDRV_PCM_ACCESS_RW_INTERLEAVED, 0) == 0) {
        long r = ioc(fd, SNDRV_PCM_IOCTL_START, NULL);
        check(r == 0, "START capture", r);
        struct { int fd; short events, revents; } pfd = { fd, POLLIN, 0 };
        r = sc(S_poll, (long)&pfd, 1, 1000, 0, 0, 0);
        struct snd_xferi x = { .buf = chunk, .frames = PERIOD };
        long rr = ioc(fd, SNDRV_PCM_IOCTL_READI_FRAMES, &x);
        out("sndtest: recorded "); outn(rr == 0 ? x.result : rr); out(" frames\n");
        check(r >= 0, "capture poll", r);
        ioc(fd, SNDRV_PCM_IOCTL_DROP, NULL);
    }
    sc(S_close, fd, 0, 0, 0, 0, 0);
}

void cmain(void)
{
    out("sndtest: start\n");
    tone_init();
    mixer();
    play_write();
    play_mmap();
    record();
    if (failed) {
        out("sndtest: "); outn(failed); out(" failed\n");
    } else
        out("sndtest: all ok\n");
    sc(S_exit, failed != 0, 0, 0, 0, 0, 0);
}
