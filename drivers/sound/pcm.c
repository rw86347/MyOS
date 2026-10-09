/*
 * pcm.c — sound streams: /dev/snd/pcmC<n>D0p (playback) and pcmC<n>D0c
 * (capture), with Linux's ALSA ioctls (asound.h).
 *
 * The sound is a ring buffer in RAM, the driver's, that the hardware plays
 * from (or records into) by DMA, round and round. Two counters say where
 * everyone is: hw_ptr, the frame the hardware reached, and appl_ptr, the
 * frame the program reached. Both count up to `boundary` (a large multiple
 * of the buffer) and wrap, so "how far apart" is a subtraction. A program
 * writes ahead of the hardware (playback) or reads behind it (capture),
 * with write()/read() (ioctls WRITEI/READI: we copy) or by mapping the
 * buffer and moving appl_ptr itself (SYNC_PTR: no copy at all).
 *
 * The hardware interrupts once per period (a few milliseconds of sound).
 * The interrupt only moves hw_ptr, notices an underrun, and wakes whoever
 * waits: no copying, no allocation, no codec commands.
 *
 * Before any of that, program and driver agree on the format: hw_params.
 * The program sends ranges ("rate 44100..48000, period about 10 ms") and
 * the kernel narrows every range against the hardware and against the
 * others (bytes = frames x frame size, buffer = period x periods, time =
 * frames / rate) until nothing changes: HW_REFINE. HW_PARAMS then picks
 * one value of each. Our own interval arithmetic, with ALSA's meanings
 * (open and closed ends, integer-only ranges).
 */
#include <errno.h>
#include <interrupts.h>
#include <kprintf.h>
#include <linux/ktime.h>
#include <mm.h>
#include <signal.h>
#include <sound/core.h>
#include <string.h>
#include <timer.h>
#include <uaccess.h>
#include <vfs.h>

#define POLLIN      0x001
#define POLLOUT     0x004
#define POLLERR     0x008
#define POLLRDNORM  0x040
#define POLLWRNORM  0x100

#define UMAX        0xffffffffu
#define LONG_MAX_U  (~0UL >> 1)

typedef struct snd_interval itv;

#define MASK(p, n)  (&(p)->masks[(n) - SNDRV_PCM_HW_PARAM_FIRST_MASK])
#define ITV(p, n)   (&(p)->intervals[(n) - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL])

/* --- interval arithmetic ---------------------------------------------------- */

/* Narrow i to its overlap with v. 1: changed, 0: not, -EINVAL: nothing left. */
static int itv_refine(itv *i, const itv *v)
{
    int changed = 0;
    if (i->empty || v->empty) {
        i->empty = 1;
        return -EINVAL;
    }
    if (i->min < v->min || (i->min == v->min && !i->openmin && v->openmin)) {
        i->min = v->min;
        i->openmin = v->openmin;
        changed = 1;
    }
    if (i->max > v->max || (i->max == v->max && !i->openmax && v->openmax)) {
        i->max = v->max;
        i->openmax = v->openmax;
        changed = 1;
    }
    if (!i->integer && v->integer) {
        i->integer = 1;
        changed = 1;
    }
    if (i->integer) {                   /* whole numbers: an open end steps in */
        if (i->openmin) {
            if (i->min == UMAX)
                goto empty;
            i->min++;
            i->openmin = 0;
        }
        if (i->openmax) {
            if (i->max == 0)
                goto empty;
            i->max--;
            i->openmax = 0;
        }
    } else if (!i->openmin && !i->openmax && i->min == i->max)
        i->integer = 1;
    if (i->min > i->max || (i->min == i->max && (i->openmin || i->openmax)))
        goto empty;
    return changed;
empty:
    i->empty = 1;
    return -EINVAL;
}

static int itv_single(const itv *i)
{
    return !i->empty && (i->min == i->max ||
                         (i->min + 1 == i->max && (i->openmin || i->openmax)));
}

/* The value of a single-valued range: (21333, 21334] is 21334. */
static unsigned int itv_value(const itv *i)
{
    return i->openmin && !i->openmax ? i->max : i->min;
}

/* Keep only the smallest values (or the largest): [min, min] — or
   [min, min + 1) when min itself is excluded, a real number just above
   it. A range that is one value already stays as it is. */
static void itv_first(itv *i)
{
    unsigned int last_max = i->max;
    if (itv_single(i))
        return;
    i->max = i->min + (i->openmin ? 1 : 0);
    i->openmax = i->openmax && i->max >= last_max;
}

static void itv_last(itv *i)
{
    unsigned int last_min = i->min;
    if (itv_single(i))
        return;
    i->min = i->max - (i->openmax ? 1 : 0);
    i->openmin = i->openmin && i->min <= last_min;
}

static unsigned int muldiv(unsigned int a, unsigned int b, unsigned int c, unsigned int *r)
{
    if (c == 0) {
        *r = 0;
        return UMAX;
    }
    uint64_t n = (uint64_t)a * b;
    *r = (unsigned int)(n % c);
    n /= c;
    return n > UMAX ? UMAX : (unsigned int)n;
}

/* c = a * b */
static void itv_mul(const itv *a, const itv *b, itv *c)
{
    uint64_t lo = (uint64_t)a->min * b->min, hi = (uint64_t)a->max * b->max;
    memset(c, 0, sizeof *c);
    c->min = lo > UMAX ? UMAX : (unsigned int)lo;
    c->max = hi > UMAX ? UMAX : (unsigned int)hi;
    c->openmin = a->openmin || b->openmin;
    c->openmax = hi > UMAX ? 0 : a->openmax || b->openmax;
    c->integer = a->integer && b->integer;
}

/* c = a / b */
static void itv_div(const itv *a, const itv *b, itv *c)
{
    unsigned int r;
    memset(c, 0, sizeof *c);
    c->min = muldiv(a->min, 1, b->max, &r);
    c->openmin = r || a->openmin || b->openmax;
    if (b->min > 0) {
        c->max = muldiv(a->max, 1, b->min, &r);
        if (r) {
            c->max = c->max == UMAX ? UMAX : c->max + 1;
            c->openmax = 1;
        } else
            c->openmax = a->openmax || b->openmin;
    } else
        c->max = UMAX;
}

/* c = a * b / k */
static void itv_muldivk(const itv *a, const itv *b, unsigned int k, itv *c)
{
    unsigned int r;
    memset(c, 0, sizeof *c);
    c->min = muldiv(a->min, b->min, k, &r);
    c->openmin = r || a->openmin || b->openmin;
    c->max = muldiv(a->max, b->max, k, &r);
    if (r) {
        c->max = c->max == UMAX ? UMAX : c->max + 1;
        c->openmax = 1;
    } else
        c->openmax = a->openmax || b->openmax;
}

/* c = a * k / b */
static void itv_mulkdiv(const itv *a, unsigned int k, const itv *b, itv *c)
{
    unsigned int r;
    memset(c, 0, sizeof *c);
    if (b->max) {
        c->min = muldiv(a->min, k, b->max, &r);
        c->openmin = r || a->openmin || b->openmax;
    }
    if (b->min) {
        c->max = muldiv(a->max, k, b->min, &r);
        if (r) {
            c->max = c->max == UMAX ? UMAX : c->max + 1;
            c->openmax = 1;
        } else
            c->openmax = a->openmax || b->openmin;
    } else
        c->max = UMAX;
}

static int itv_set(itv *i, unsigned int min, unsigned int max, int integer)
{
    itv v = { .min = min, .max = max, .integer = (unsigned int)integer };
    return itv_refine(i, &v);
}

/* Only the values in list[] (ascending). */
static int itv_list(itv *i, const unsigned int *list, unsigned int n)
{
    itv v = { .min = UMAX, .max = 0, .integer = 1 };
    for (unsigned int k = 0; k < n; k++) {
        unsigned int x = list[k];
        if (x < i->min || (x == i->min && i->openmin) || x > i->max ||
            (x == i->max && i->openmax))
            continue;
        if (x < v.min)
            v.min = x;
        if (x > v.max)
            v.max = x;
    }
    if (v.min > v.max) {
        i->empty = 1;
        return -EINVAL;
    }
    return itv_refine(i, &v);
}

/* Multiples of step only. */
static int itv_step(itv *i, unsigned int step)
{
    itv v = *i;
    if (i->empty || (i->openmax && i->max == 0) || itv_value(i) > UMAX - step) {
        i->empty = 1;
        return -EINVAL;
    }
    unsigned int lo = itv_value(i), hi = i->openmax ? i->max - 1 : i->max;
    v.min = (lo + step - 1) / step * step;
    v.max = hi / step * step;
    v.openmin = v.openmax = 0;
    if (v.min > v.max) {
        i->empty = 1;
        return -EINVAL;
    }
    return itv_refine(i, &v);
}

static int mask_refine(struct snd_mask *m, uint64_t allowed)
{
    uint32_t lo = m->bits[0] & (uint32_t)allowed, hi = m->bits[1] & (uint32_t)(allowed >> 32);
    int changed = lo != m->bits[0] || hi != m->bits[1];
    for (int k = 2; k < 8; k++)
        if (m->bits[k]) {
            m->bits[k] = 0;
            changed = 1;
        }
    m->bits[0] = lo;
    m->bits[1] = hi;
    return lo | hi ? changed : -EINVAL;
}

static uint64_t mask_get(const struct snd_mask *m)
{
    return m->bits[0] | (uint64_t)m->bits[1] << 32;
}

static int format_bits(int f)
{
    return f == SNDRV_PCM_FORMAT_S16_LE ? 16 : f == SNDRV_PCM_FORMAT_S32_LE ? 32 : 0;
}

/* --- hw_params: narrow every range until nothing changes ----------------------- */

static int refine(struct snd_pcm_substream *s, struct snd_pcm_hw_params *p)
{
    const struct snd_pcm_hardware *hw = &s->hw;
    struct snd_pcm_hw_params old = *p;
    itv t;
    int err = 0;
#define R(x) do { if ((err = (x)) < 0) return err; } while (0)

    /* What the hardware does. */
    R(mask_refine(MASK(p, SNDRV_PCM_HW_PARAM_ACCESS),
                  1u << SNDRV_PCM_ACCESS_MMAP_INTERLEAVED | 1u << SNDRV_PCM_ACCESS_RW_INTERLEAVED));
    R(mask_refine(MASK(p, SNDRV_PCM_HW_PARAM_FORMAT), hw->formats));
    R(mask_refine(MASK(p, SNDRV_PCM_HW_PARAM_SUBFORMAT), 1u << SNDRV_PCM_SUBFORMAT_STD));
    R(itv_set(ITV(p, SNDRV_PCM_HW_PARAM_SAMPLE_BITS), 8, 64, 1));
    R(itv_set(ITV(p, SNDRV_PCM_HW_PARAM_FRAME_BITS), 8, 1024, 1));
    R(itv_set(ITV(p, SNDRV_PCM_HW_PARAM_CHANNELS), hw->channels_min, hw->channels_max, 1));
    R(itv_list(ITV(p, SNDRV_PCM_HW_PARAM_RATE), hw->rates, hw->nrates));
    R(itv_set(ITV(p, SNDRV_PCM_HW_PARAM_PERIOD_BYTES), hw->period_bytes_min,
              hw->period_bytes_max, 1));
    R(itv_step(ITV(p, SNDRV_PCM_HW_PARAM_PERIOD_BYTES), hw->step_bytes));
    R(itv_set(ITV(p, SNDRV_PCM_HW_PARAM_BUFFER_BYTES), hw->period_bytes_min * hw->periods_min,
              hw->buffer_bytes_max, 1));
    R(itv_step(ITV(p, SNDRV_PCM_HW_PARAM_BUFFER_BYTES), hw->step_bytes));
    R(itv_set(ITV(p, SNDRV_PCM_HW_PARAM_PERIODS), hw->periods_min, hw->periods_max, 1));
    R(itv_set(ITV(p, SNDRV_PCM_HW_PARAM_PERIOD_SIZE), 1, UMAX, 1));
    R(itv_set(ITV(p, SNDRV_PCM_HW_PARAM_BUFFER_SIZE), 1, UMAX, 1));

    /* How the ranges depend on each other. Each pass applies every rule; at
       most 16 passes (a few hundred multiplications), and in practice
       three or four. */
    for (int pass = 0; pass < 16; pass++) {
        int changed = 0;
        itv *sb = ITV(p, SNDRV_PCM_HW_PARAM_SAMPLE_BITS), *fb = ITV(p, SNDRV_PCM_HW_PARAM_FRAME_BITS);
        itv *ch = ITV(p, SNDRV_PCM_HW_PARAM_CHANNELS), *rate = ITV(p, SNDRV_PCM_HW_PARAM_RATE);
        itv *pt = ITV(p, SNDRV_PCM_HW_PARAM_PERIOD_TIME), *ps = ITV(p, SNDRV_PCM_HW_PARAM_PERIOD_SIZE);
        itv *pb = ITV(p, SNDRV_PCM_HW_PARAM_PERIOD_BYTES), *np = ITV(p, SNDRV_PCM_HW_PARAM_PERIODS);
        itv *bt = ITV(p, SNDRV_PCM_HW_PARAM_BUFFER_TIME), *bs = ITV(p, SNDRV_PCM_HW_PARAM_BUFFER_SIZE);
        itv *bb = ITV(p, SNDRV_PCM_HW_PARAM_BUFFER_BYTES);
        struct snd_mask *fmt = MASK(p, SNDRV_PCM_HW_PARAM_FORMAT);
#define RULE(target, expr) do { expr; R(itv_refine(target, &t)); changed |= err; } while (0)

        /* sample bits <-> format */
        uint64_t keep = 0;
        unsigned int lo = UMAX, hi = 0;
        for (int f = 0; f < 64; f++) {
            int b = format_bits(f);
            if (!(mask_get(fmt) >> f & 1) || !b)
                continue;
            if ((unsigned int)b < sb->min || (unsigned int)b > sb->max)
                continue;
            keep |= 1ull << f;
            lo = (unsigned int)b < lo ? (unsigned int)b : lo;
            hi = (unsigned int)b > hi ? (unsigned int)b : hi;
        }
        R(mask_refine(fmt, keep));
        changed |= err;
        R(itv_set(sb, lo, hi, 1));
        changed |= err;

        RULE(fb, itv_mul(sb, ch, &t));
        RULE(sb, itv_div(fb, ch, &t));
        RULE(ch, itv_div(fb, sb, &t));
        RULE(fb, itv_mulkdiv(pb, 8, ps, &t));
        RULE(fb, itv_mulkdiv(bb, 8, bs, &t));
        RULE(ps, itv_mulkdiv(pb, 8, fb, &t));
        RULE(bs, itv_mulkdiv(bb, 8, fb, &t));
        RULE(pb, itv_muldivk(ps, fb, 8, &t));
        RULE(bb, itv_muldivk(bs, fb, 8, &t));
        RULE(bs, itv_mul(ps, np, &t));
        RULE(ps, itv_div(bs, np, &t));
        RULE(np, itv_div(bs, ps, &t));
        RULE(ps, itv_muldivk(pt, rate, 1000000, &t));
        RULE(bs, itv_muldivk(bt, rate, 1000000, &t));
        RULE(pt, itv_mulkdiv(ps, 1000000, rate, &t));
        RULE(bt, itv_mulkdiv(bs, 1000000, rate, &t));
        RULE(rate, itv_mulkdiv(ps, 1000000, pt, &t));
        RULE(rate, itv_mulkdiv(bs, 1000000, bt, &t));
        R(itv_list(rate, s->hw.rates, s->hw.nrates));
        changed |= err;
        R(itv_step(pb, hw->step_bytes));
        changed |= err;
        R(itv_step(bb, hw->step_bytes));
        changed |= err;
        if (!changed)
            break;
#undef RULE
    }
#undef R

    /* Which ones changed (alsa-lib looks), and the read-only answers. */
    p->cmask = 0;
    for (int k = 0; k < 3; k++)
        if (memcmp(&p->masks[k], &old.masks[k], sizeof p->masks[k]))
            p->cmask |= 1u << k;
    for (int k = 0; k < 12; k++)
        if (memcmp(&p->intervals[k], &old.intervals[k], sizeof p->intervals[k]))
            p->cmask |= 1u << (k + SNDRV_PCM_HW_PARAM_FIRST_INTERVAL);
    p->rmask = 0;
    p->info = hw->info;
    p->fifo_size = 0;
    itv *sb = ITV(p, SNDRV_PCM_HW_PARAM_SAMPLE_BITS), *rate = ITV(p, SNDRV_PCM_HW_PARAM_RATE);
    p->msbits = itv_single(sb) ? (itv_value(sb) == 32 && hw->msbits ? hw->msbits : itv_value(sb)) : 0;
    if (itv_single(rate)) {
        p->rate_num = itv_value(rate);
        p->rate_den = 1;
    } else
        p->rate_num = p->rate_den = 0;
    return 0;
}

/* HW_PARAMS: one value each, in Linux's order — the smallest of everything
   except the buffer, which is the largest. */
static int choose(struct snd_pcm_substream *s, struct snd_pcm_hw_params *p)
{
    static const int order[] = {
        SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_HW_PARAM_SUBFORMAT,
        SNDRV_PCM_HW_PARAM_CHANNELS, SNDRV_PCM_HW_PARAM_RATE, SNDRV_PCM_HW_PARAM_PERIOD_TIME,
        SNDRV_PCM_HW_PARAM_BUFFER_SIZE, SNDRV_PCM_HW_PARAM_TICK_TIME,
    };
    int err = refine(s, p);
    for (unsigned int k = 0; !err && k < sizeof order / sizeof *order; k++) {
        int v = order[k];
        if (v <= SNDRV_PCM_HW_PARAM_LAST_MASK) {
            uint64_t m = mask_get(MASK(p, v));
            mask_refine(MASK(p, v), m & -m);        /* the lowest bit */
        } else if (v == SNDRV_PCM_HW_PARAM_BUFFER_SIZE)
            itv_last(ITV(p, v));
        else
            itv_first(ITV(p, v));
        err = refine(s, p);
    }
    return err;
}

/* --- the stream's position ----------------------------------------------------- */

static int running(struct snd_pcm_runtime *r)
{
    return r->state == SNDRV_PCM_STATE_RUNNING || r->state == SNDRV_PCM_STATE_DRAINING;
}

/* Playback: room to write. Capture: frames to read. */
static snd_pcm_uframes_t avail(struct snd_pcm_substream *s)
{
    struct snd_pcm_runtime *r = &s->runtime;
    long a = s->stream == SNDRV_PCM_STREAM_PLAYBACK
           ? (long)(r->hw_ptr + r->buffer_size) - (long)r->appl_ptr
           : (long)r->hw_ptr - (long)r->appl_ptr;
    if (a < 0)
        a += (long)r->boundary;
    else if ((snd_pcm_uframes_t)a >= r->boundary)
        a -= (long)r->boundary;
    return (snd_pcm_uframes_t)a;
}

/* Frames between the hardware and the program: playback still to play,
   capture recorded but not read. */
static snd_pcm_uframes_t hw_avail(struct snd_pcm_substream *s)
{
    snd_pcm_uframes_t a = avail(s);
    return s->stream == SNDRV_PCM_STREAM_PLAYBACK ? s->runtime.buffer_size - a : a;
}

static void now(struct snd_timespec *ts)
{
    uint64_t ns = ktime_get_ns();
    ts->tv_sec = (int64_t)(ns / 1000000000u);
    ts->tv_nsec = (long)(ns % 1000000000u);
}

static void stop(struct snd_pcm_substream *s, int state)
{
    struct snd_pcm_runtime *r = &s->runtime;
    if (running(r) || r->state == SNDRV_PCM_STATE_PAUSED)
        s->ops->trigger(s, SNDRV_PCM_TRIGGER_STOP);
    r->state = state;
    now(&r->trigger_tstamp);
    wq_wake_all(&r->sleep);
    wq_wake_all(&poll_wait);
}

/* Read where the hardware is (interrupts off). The position is inside the
   buffer; a smaller one than last time means it went round. */
static void update(struct snd_pcm_substream *s)
{
    struct snd_pcm_runtime *r = &s->runtime;
    if (!running(r))
        return;
    snd_pcm_uframes_t pos = s->ops->pointer(s);
    if (pos >= r->buffer_size)
        pos = 0;
    snd_pcm_uframes_t base = r->hw_ptr_base, hw = base + pos;
    if (hw < r->hw_ptr) {
        base += r->buffer_size;
        if (base >= r->boundary)
            base = 0;
        hw = base + pos;
    }
    r->hw_ptr_base = base;
    r->hw_ptr = hw;
    snd_pcm_uframes_t a = avail(s);
    if (a > r->avail_max)
        r->avail_max = a;
    if (s->stream == SNDRV_PCM_STREAM_PLAYBACK && r->state == SNDRV_PCM_STATE_DRAINING) {
        if (a >= r->buffer_size)
            stop(s, SNDRV_PCM_STATE_SETUP);         /* all played */
    } else if (a >= r->stop_threshold)
        stop(s, SNDRV_PCM_STATE_XRUN);              /* under- or overrun */
}

void snd_pcm_period_elapsed(struct snd_pcm_substream *s)
{
    struct snd_pcm_runtime *r = &s->runtime;
    if (!s->opened || !running(r))
        return;
    update(s);
    wq_wake_all(&r->sleep);
    wq_wake_all(&poll_wait);
}

/* HWSYNC: bring hw_ptr up to date, and say if the stream is broken
   (Linux's answers, state by state). Interrupts off. */
static int hwsync(struct snd_pcm_substream *s)
{
    switch (s->runtime.state) {
    case SNDRV_PCM_STATE_DRAINING:
        if (s->stream == SNDRV_PCM_STREAM_CAPTURE)
            return -EBADFD;
        /* fall through */
    case SNDRV_PCM_STATE_RUNNING:
        update(s);
        return s->runtime.state == SNDRV_PCM_STATE_XRUN ? -EPIPE : 0;
    case SNDRV_PCM_STATE_PREPARED:
    case SNDRV_PCM_STATE_PAUSED:
        return 0;
    case SNDRV_PCM_STATE_XRUN:
        return -EPIPE;
    }
    return -EBADFD;
}

static int start(struct snd_pcm_substream *s)
{
    struct snd_pcm_runtime *r = &s->runtime;
    if (r->state != SNDRV_PCM_STATE_PREPARED)
        return -EBADFD;
    if (s->stream == SNDRV_PCM_STREAM_PLAYBACK && !hw_avail(s))
        return -EPIPE;                              /* nothing to play */
    int err = s->ops->trigger(s, SNDRV_PCM_TRIGGER_START);
    if (err < 0)
        return err;
    r->state = SNDRV_PCM_STATE_RUNNING;
    now(&r->trigger_tstamp);
    return 0;
}

/* Wait for the next period, at most 10 s (Linux's limit: then the DMA or
   the interrupt is broken). Interrupts off. */
static int wait_period(struct snd_pcm_substream *s)
{
    if (s->runtime.state == SNDRV_PCM_STATE_PAUSED) {
        wq_sleep(&s->runtime.sleep);                /* until resumed, however long */
        return signal_pending() ? -EINTR : 0;
    }
    if (wq_sleep_until(&s->runtime.sleep, timer_ms() + 10000)) {
        kprintf("sound: %s: no interrupt for 10 s\n",
                s->stream == SNDRV_PCM_STREAM_PLAYBACK ? "playback" : "capture");
        stop(s, SNDRV_PCM_STATE_XRUN);
        return -EIO;
    }
    return signal_pending() ? -EINTR : 0;
}

/* --- read() and write() of frames (WRITEI / READI) ----------------------------- */

static long transfer(struct snd_pcm_substream *s, struct file *f, uint8_t *ubuf,
                     snd_pcm_uframes_t frames)
{
    struct snd_pcm_runtime *r = &s->runtime;
    int play = s->stream == SNDRV_PCM_STREAM_PLAYBACK;
    snd_pcm_uframes_t done = 0;
    long err = 0;
    if (r->access != SNDRV_PCM_ACCESS_RW_INTERLEAVED)
        return -EINVAL;
    unsigned long flags = irq_save();
    if (!play && r->state == SNDRV_PCM_STATE_PREPARED && frames >= r->start_threshold)
        err = start(s);
    while (!err && done < frames) {
        switch (r->state) {
        case SNDRV_PCM_STATE_PREPARED:
        case SNDRV_PCM_STATE_RUNNING:
        case SNDRV_PCM_STATE_PAUSED:
            break;
        case SNDRV_PCM_STATE_XRUN:
            err = -EPIPE;
            continue;
        case SNDRV_PCM_STATE_DRAINING:
            if (!play)
                break;                              /* capture: what is left */
            err = -EBADFD;
            continue;
        default:
            err = -EBADFD;
            continue;
        }
        update(s);
        snd_pcm_uframes_t a = avail(s), want = frames - done;
        /* Wait for room (or sound) only when there is none, or less than
           both the rest and avail_min — so a writer wakes once a period,
           not once a frame. A stream not yet started never waits: it is
           started instead. */
        if (a == 0 || (a < want && a < r->avail_min && r->state != SNDRV_PCM_STATE_PREPARED)) {
            if (!play && r->state == SNDRV_PCM_STATE_DRAINING) {
                stop(s, SNDRV_PCM_STATE_SETUP);     /* recorded sound all read */
                break;
            }
            if (r->state == SNDRV_PCM_STATE_PREPARED) {
                err = start(s);                     /* full (or empty): go */
                continue;
            }
            if (f->flags & O_NONBLOCK) {
                err = -EAGAIN;
                continue;
            }
            err = wait_period(s);
            continue;
        }
        /* Copy the part up to the buffer's end, with interrupts on (the
           copy may fault in the program's page). */
        snd_pcm_uframes_t off = r->appl_ptr % r->buffer_size, n = want < a ? want : a;
        if (n > r->buffer_size - off)
            n = r->buffer_size - off;
        uint32_t bytes = (uint32_t)(n * r->frame_bytes);
        uint8_t *dma = r->dma_area + off * r->frame_bytes, *u = ubuf + done * r->frame_bytes;
        irq_restore(flags);
        int e = play ? copy_from_user(dma, u, bytes) : copy_to_user(u, dma, bytes);
        flags = irq_save();
        if (e < 0) {
            err = -EFAULT;
            continue;
        }
        r->appl_ptr += n;
        if (r->appl_ptr >= r->boundary)
            r->appl_ptr -= r->boundary;
        done += n;
        if (play && r->state == SNDRV_PCM_STATE_PREPARED && hw_avail(s) >= r->start_threshold)
            err = start(s);
    }
    irq_restore(flags);
    return done ? (long)done : err;
}

/* --- the ioctls ------------------------------------------------------------------ */

int snd_pcm_info_fill(struct snd_pcm_substream *s, struct snd_pcm_info *info)
{
    memset(info, 0, sizeof *info);
    info->stream = s->stream;
    info->card = s->pcm->card->number;
    strlcpy((char *)info->id, s->pcm->id, sizeof info->id);
    strlcpy((char *)info->name, s->pcm->name, sizeof info->name);
    memcpy(info->subname, "subdevice #0", 13);
    info->subdevices_count = 1;
    info->subdevices_avail = s->opened ? 0 : 1;
    return 0;
}

static int hw_params(struct snd_pcm_substream *s, struct snd_pcm_hw_params *p)
{
    struct snd_pcm_runtime *r = &s->runtime;
    if (r->state != SNDRV_PCM_STATE_OPEN && r->state != SNDRV_PCM_STATE_SETUP &&
        r->state != SNDRV_PCM_STATE_PREPARED)
        return -EBADFD;
    int err = choose(s, p);
    if (err < 0)
        return err;
    uint64_t acc = mask_get(MASK(p, SNDRV_PCM_HW_PARAM_ACCESS));
    uint64_t fmt = mask_get(MASK(p, SNDRV_PCM_HW_PARAM_FORMAT));
    r->access = (unsigned int)__builtin_ctzll(acc);
    r->format = (unsigned int)__builtin_ctzll(fmt);
    r->channels = itv_value(ITV(p, SNDRV_PCM_HW_PARAM_CHANNELS));
    r->rate = itv_value(ITV(p, SNDRV_PCM_HW_PARAM_RATE));
    r->period_size = itv_value(ITV(p, SNDRV_PCM_HW_PARAM_PERIOD_SIZE));
    r->periods = itv_value(ITV(p, SNDRV_PCM_HW_PARAM_PERIODS));
    r->buffer_size = itv_value(ITV(p, SNDRV_PCM_HW_PARAM_BUFFER_SIZE));
    r->sample_bits = (unsigned int)format_bits((int)r->format);
    r->frame_bytes = r->sample_bits / 8 * r->channels;
    if (r->buffer_size * r->frame_bytes > r->dma_bytes || r->period_size * r->periods != r->buffer_size)
        return -EINVAL;
    if (s->ops->hw_params && (err = s->ops->hw_params(s)) < 0)
        return err;
    /* sw_params' defaults */
    r->avail_min = r->period_size;
    r->start_threshold = 1;
    r->stop_threshold = r->buffer_size;
    r->boundary = r->buffer_size;
    while (r->boundary * 2 <= LONG_MAX_U - r->buffer_size)
        r->boundary *= 2;
    r->state = SNDRV_PCM_STATE_SETUP;
    return 0;
}

static int sw_params(struct snd_pcm_substream *s, struct snd_pcm_sw_params *p)
{
    struct snd_pcm_runtime *r = &s->runtime;
    if (r->state == SNDRV_PCM_STATE_OPEN)
        return -EBADFD;
    if (p->avail_min == 0 || p->tstamp_mode < 0 || p->tstamp_mode > 1 || p->tstamp_type > 2)
        return -EINVAL;
    if (p->silence_size >= r->boundary ? p->silence_threshold != 0
        : p->silence_size > p->silence_threshold || p->silence_threshold > r->buffer_size)
        return -EINVAL;
    unsigned long flags = irq_save();
    r->tstamp_mode = p->tstamp_mode;
    r->tstamp_type = p->tstamp_type;
    r->avail_min = p->avail_min;
    r->start_threshold = p->start_threshold;
    r->stop_threshold = p->stop_threshold;
    p->boundary = r->boundary;
    update(s);
    irq_restore(flags);
    return 0;
}

static void fill_status(struct snd_pcm_substream *s, struct snd_pcm_status *st)
{
    struct snd_pcm_runtime *r = &s->runtime;
    memset(st, 0, sizeof *st);
    update(s);
    st->state = r->state;
    st->trigger_tstamp = r->trigger_tstamp;
    now(&st->tstamp);
    st->appl_ptr = r->appl_ptr;
    st->hw_ptr = r->hw_ptr;
    if (r->state != SNDRV_PCM_STATE_OPEN) {
        st->avail = avail(s);
        st->delay = running(r) || r->state == SNDRV_PCM_STATE_PAUSED
                  ? (snd_pcm_sframes_t)hw_avail(s) : 0;
    }
    st->avail_max = r->avail_max > st->avail ? r->avail_max : st->avail;
    r->avail_max = 0;
    st->suspended_state = SNDRV_PCM_STATE_OPEN;
    st->audio_tstamp = st->tstamp;
}

static int drain(struct snd_pcm_substream *s, struct file *f)
{
    struct snd_pcm_runtime *r = &s->runtime;
    unsigned long flags = irq_save();
    int err = 0;
    if (r->state == SNDRV_PCM_STATE_OPEN || r->state == SNDRV_PCM_STATE_DISCONNECTED)
        err = -EBADFD;
    else if (s->stream == SNDRV_PCM_STREAM_CAPTURE) {
        if (running(r))         /* stop recording; what is recorded can still be read */
            stop(s, hw_avail(s) ? SNDRV_PCM_STATE_DRAINING : SNDRV_PCM_STATE_SETUP);
    } else {
        if (r->state == SNDRV_PCM_STATE_PREPARED && hw_avail(s))
            err = start(s);
        if (!err && r->state == SNDRV_PCM_STATE_RUNNING) {
            /* Silence after the last frame: the DMA runs on until the next
               period's interrupt notices the end, and must not replay old
               sound meanwhile. */
            snd_pcm_uframes_t free = avail(s), off = r->appl_ptr % r->buffer_size;
            for (snd_pcm_uframes_t k = 0; k < free; k++)
                memset(r->dma_area + ((off + k) % r->buffer_size) * r->frame_bytes, 0,
                       r->frame_bytes);
            r->state = SNDRV_PCM_STATE_DRAINING;
        } else if (!err && r->state != SNDRV_PCM_STATE_DRAINING &&
                   r->state != SNDRV_PCM_STATE_PAUSED)
            r->state = SNDRV_PCM_STATE_SETUP;
        while (!err && r->state == SNDRV_PCM_STATE_DRAINING) {
            if (f->flags & O_NONBLOCK)
                err = -EAGAIN;
            else
                err = wait_period(s);
        }
    }
    irq_restore(flags);
    return err;
}

static int32_t pcm_ioctl(struct file *f, uint32_t cmd, uintptr_t arg)
{
    struct snd_pcm_substream *s = f->priv;
    struct snd_pcm_runtime *r = &s->runtime;
    unsigned long flags;
    int err = 0;

    switch (cmd) {
    case SNDRV_PCM_IOCTL_PVERSION: {
        int v = SNDRV_PCM_VERSION;
        return copy_to_user((void *)arg, &v, sizeof v);
    }
    case SNDRV_PCM_IOCTL_INFO: {
        struct snd_pcm_info info;
        snd_pcm_info_fill(s, &info);
        return copy_to_user((void *)arg, &info, sizeof info);
    }
    case SNDRV_PCM_IOCTL_TSTAMP:
    case SNDRV_PCM_IOCTL_TTSTAMP:
    case SNDRV_PCM_IOCTL_USER_PVERSION:
        return 0;
    case SNDRV_PCM_IOCTL_HW_REFINE:
    case SNDRV_PCM_IOCTL_HW_PARAMS: {
        struct snd_pcm_hw_params p;
        if (copy_from_user(&p, (void *)arg, sizeof p) < 0)
            return -EFAULT;
        err = cmd == SNDRV_PCM_IOCTL_HW_REFINE ? refine(s, &p) : hw_params(s, &p);
        if (err < 0)
            return err;
        return copy_to_user((void *)arg, &p, sizeof p);
    }
    case SNDRV_PCM_IOCTL_HW_FREE:
        if (running(r) || r->state == SNDRV_PCM_STATE_PAUSED)
            return -EBADFD;
        if (s->ops->hw_free)
            s->ops->hw_free(s);
        r->state = SNDRV_PCM_STATE_OPEN;
        return 0;
    case SNDRV_PCM_IOCTL_SW_PARAMS: {
        struct snd_pcm_sw_params p;
        if (copy_from_user(&p, (void *)arg, sizeof p) < 0)
            return -EFAULT;
        if ((err = sw_params(s, &p)) < 0)
            return err;
        return copy_to_user((void *)arg, &p, sizeof p);
    }
    case SNDRV_PCM_IOCTL_STATUS:
    case SNDRV_PCM_IOCTL_STATUS_EXT: {
        struct snd_pcm_status st;
        flags = irq_save();
        fill_status(s, &st);
        irq_restore(flags);
        return copy_to_user((void *)arg, &st, sizeof st);
    }
    case SNDRV_PCM_IOCTL_DELAY: {
        flags = irq_save();
        update(s);
        snd_pcm_sframes_t d = (snd_pcm_sframes_t)hw_avail(s);
        int state = r->state;
        irq_restore(flags);
        if (state == SNDRV_PCM_STATE_XRUN)
            return -EPIPE;
        if (state != SNDRV_PCM_STATE_RUNNING && state != SNDRV_PCM_STATE_DRAINING &&
            state != SNDRV_PCM_STATE_PREPARED && state != SNDRV_PCM_STATE_PAUSED)
            return -EBADFD;
        return copy_to_user((void *)arg, &d, sizeof d);
    }
    case SNDRV_PCM_IOCTL_HWSYNC:
        flags = irq_save();
        err = hwsync(s);
        irq_restore(flags);
        return err;
    case SNDRV_PCM_IOCTL_SYNC_PTR: {
        /* The mmap user's way to move appl_ptr and learn hw_ptr. */
        struct snd_pcm_sync_ptr sp;
        if (copy_from_user(&sp, (void *)arg, sizeof sp) < 0)
            return -EFAULT;
        flags = irq_save();
        if ((sp.flags & SNDRV_PCM_SYNC_PTR_HWSYNC) && (err = hwsync(s)) < 0) {
            irq_restore(flags);
            return err;
        }
        if (!(sp.flags & SNDRV_PCM_SYNC_PTR_APPL) && sp.c.control.appl_ptr < r->boundary)
            r->appl_ptr = sp.c.control.appl_ptr;
        if (!(sp.flags & SNDRV_PCM_SYNC_PTR_AVAIL_MIN) && sp.c.control.avail_min)
            r->avail_min = sp.c.control.avail_min;
        memset(&sp.s, 0, sizeof sp.s);
        sp.s.status.state = r->state;
        sp.s.status.hw_ptr = r->hw_ptr;
        now(&sp.s.status.tstamp);
        sp.s.status.suspended_state = SNDRV_PCM_STATE_OPEN;
        sp.s.status.audio_tstamp = sp.s.status.tstamp;
        sp.c.control.appl_ptr = r->appl_ptr;
        sp.c.control.avail_min = r->avail_min;
        irq_restore(flags);
        return copy_to_user((void *)arg, &sp, sizeof sp);
    }
    case SNDRV_PCM_IOCTL_CHANNEL_INFO: {
        struct snd_pcm_channel_info ci;
        if (copy_from_user(&ci, (void *)arg, sizeof ci) < 0)
            return -EFAULT;
        if (r->state == SNDRV_PCM_STATE_OPEN)
            return -EBADFD;
        if (ci.channel >= r->channels)
            return -EINVAL;
        ci.offset = 0;
        ci.first = ci.channel * r->sample_bits;
        ci.step = r->channels * r->sample_bits;
        return copy_to_user((void *)arg, &ci, sizeof ci);
    }
    case SNDRV_PCM_IOCTL_PREPARE:
        flags = irq_save();
        if (r->state == SNDRV_PCM_STATE_OPEN || r->state == SNDRV_PCM_STATE_DISCONNECTED)
            err = -EBADFD;
        else if (r->state == SNDRV_PCM_STATE_RUNNING ||
                 (r->state == SNDRV_PCM_STATE_DRAINING && s->stream == SNDRV_PCM_STREAM_PLAYBACK))
            err = -EBUSY;
        else {
            if (r->state == SNDRV_PCM_STATE_PAUSED || r->state == SNDRV_PCM_STATE_DRAINING)
                stop(s, SNDRV_PCM_STATE_SETUP);
            r->hw_ptr = r->appl_ptr = r->hw_ptr_base = r->avail_max = 0;
            err = s->ops->prepare ? s->ops->prepare(s) : 0;
            if (!err)
                r->state = SNDRV_PCM_STATE_PREPARED;
        }
        irq_restore(flags);
        return err;
    case SNDRV_PCM_IOCTL_RESET:
        flags = irq_save();
        if (r->state == SNDRV_PCM_STATE_RUNNING || r->state == SNDRV_PCM_STATE_PREPARED ||
            r->state == SNDRV_PCM_STATE_PAUSED) {
            update(s);
            r->appl_ptr = r->hw_ptr;
        } else
            err = -EBADFD;
        irq_restore(flags);
        return err;
    case SNDRV_PCM_IOCTL_START:
        flags = irq_save();
        err = start(s);
        irq_restore(flags);
        return err;
    case SNDRV_PCM_IOCTL_DROP:
        flags = irq_save();
        if (r->state == SNDRV_PCM_STATE_OPEN || r->state == SNDRV_PCM_STATE_DISCONNECTED)
            err = -EBADFD;
        else if (r->state != SNDRV_PCM_STATE_SETUP)
            stop(s, SNDRV_PCM_STATE_SETUP);
        irq_restore(flags);
        return err;
    case SNDRV_PCM_IOCTL_DRAIN:
        return drain(s, f);
    case SNDRV_PCM_IOCTL_PAUSE: {
        int push = (int)arg;
        flags = irq_save();
        if (push && r->state == SNDRV_PCM_STATE_RUNNING) {
            update(s);
            if ((err = s->ops->trigger(s, SNDRV_PCM_TRIGGER_PAUSE_PUSH)) == 0)
                r->state = SNDRV_PCM_STATE_PAUSED;
        } else if (!push && r->state == SNDRV_PCM_STATE_PAUSED) {
            if ((err = s->ops->trigger(s, SNDRV_PCM_TRIGGER_PAUSE_RELEASE)) == 0)
                r->state = SNDRV_PCM_STATE_RUNNING;
        } else
            err = -EBADFD;
        now(&r->trigger_tstamp);
        irq_restore(flags);
        return err;
    }
    case SNDRV_PCM_IOCTL_REWIND:
    case SNDRV_PCM_IOCTL_FORWARD: {
        snd_pcm_uframes_t n;
        if (copy_from_user(&n, (void *)arg, sizeof n) < 0)
            return -EFAULT;
        flags = irq_save();
        update(s);
        if (r->state == SNDRV_PCM_STATE_OPEN || r->state == SNDRV_PCM_STATE_SETUP)
            err = -EBADFD;
        else if (r->state == SNDRV_PCM_STATE_XRUN)
            err = -EPIPE;
        else {
            int back = cmd == SNDRV_PCM_IOCTL_REWIND;
            /* back: at most what is still the program's (queued, or read);
               forward: at most the room (or what was recorded) */
            snd_pcm_uframes_t lim = back ? hw_avail(s) : avail(s);
            if (back && s->stream == SNDRV_PCM_STREAM_CAPTURE)
                lim = r->buffer_size - avail(s);
            if (n > lim)
                n = lim;
            r->appl_ptr = back ? (r->appl_ptr >= n ? r->appl_ptr - n : r->appl_ptr + r->boundary - n)
                               : (r->appl_ptr + n >= r->boundary ? r->appl_ptr + n - r->boundary
                                                                 : r->appl_ptr + n);
        }
        irq_restore(flags);
        if (err < 0)
            return err;
        return copy_to_user((void *)arg, &n, sizeof n);    /* how far it went */
    }
    case SNDRV_PCM_IOCTL_XRUN:
        flags = irq_save();
        if (running(r) || r->state == SNDRV_PCM_STATE_PAUSED)
            stop(s, SNDRV_PCM_STATE_XRUN);
        else
            err = -EBADFD;
        irq_restore(flags);
        return err;
    case SNDRV_PCM_IOCTL_WRITEI_FRAMES:
    case SNDRV_PCM_IOCTL_READI_FRAMES: {
        struct snd_xferi x;
        if ((cmd == SNDRV_PCM_IOCTL_WRITEI_FRAMES) != (s->stream == SNDRV_PCM_STREAM_PLAYBACK))
            return -EINVAL;
        if (copy_from_user(&x, (void *)arg, sizeof x) < 0)
            return -EFAULT;
        long n = transfer(s, f, x.buf, x.frames);
        x.result = n;
        if (copy_to_user((void *)arg, &x, sizeof x) < 0)
            return -EFAULT;
        return n < 0 ? (int)n : 0;
    }
    case SNDRV_PCM_IOCTL_RESUME:
        return -ENOSYS;
    case SNDRV_PCM_IOCTL_LINK:
    case SNDRV_PCM_IOCTL_UNLINK:
        return -EOPNOTSUPP;
    }
    return -ENOTTY;
}

/* --- open, close, mmap, poll -------------------------------------------------------- */

static int pcm_open(struct file *f)
{
    unsigned int minor = f->inode->rdev & 0xff;
    struct snd_card *card = snd_card_get((int)(minor / 32));
    int stream = minor % 32 == 24 ? SNDRV_PCM_STREAM_CAPTURE : SNDRV_PCM_STREAM_PLAYBACK;
    if (!card || !card->pcm || !card->pcm->has[stream])
        return -ENODEV;
    struct snd_pcm_substream *s = &card->pcm->streams[stream];
    unsigned long flags = irq_save();
    if (s->opened) {
        irq_restore(flags);
        return -EBUSY;                  /* one program per stream (a sound
                                           server mixes for the others) */
    }
    s->opened = 1;
    irq_restore(flags);
    struct snd_pcm_runtime *r = &s->runtime;
    r->state = SNDRV_PCM_STATE_OPEN;
    r->hw_ptr = r->appl_ptr = r->hw_ptr_base = r->avail_max = 0;
    int err = s->ops->open ? s->ops->open(s) : 0;
    if (err < 0) {
        s->opened = 0;
        return err;
    }
    f->priv = s;
    return 0;
}

static void pcm_release(struct file *f)
{
    struct snd_pcm_substream *s = f->priv;
    int had_params = s->runtime.state != SNDRV_PCM_STATE_OPEN;
    unsigned long flags = irq_save();
    if (running(&s->runtime) || s->runtime.state == SNDRV_PCM_STATE_PAUSED)
        stop(s, SNDRV_PCM_STATE_SETUP);
    irq_restore(flags);
    if (had_params && s->ops->hw_free)
        s->ops->hw_free(s);
    if (s->ops->close)
        s->ops->close(s);
    s->runtime.state = SNDRV_PCM_STATE_OPEN;
    s->opened = 0;
}

/* Only the sound itself can be mapped. The status and control pages are
   not offered: alsa-lib then exchanges them with SYNC_PTR, which works the
   same on both CPUs (on i386, musl's 64-bit time makes the mapped layout
   ambiguous). */
static int pcm_mmap(struct file *f, struct vma *vma)
{
    struct snd_pcm_substream *s = f->priv;
    struct snd_pcm_runtime *r = &s->runtime;
    if (vma->file_off != SNDRV_PCM_MMAP_OFFSET_DATA)
        return -ENXIO;
    if (r->state == SNDRV_PCM_STATE_OPEN)
        return -EBADFD;
    if (r->access != SNDRV_PCM_ACCESS_MMAP_INTERLEAVED)
        return -EINVAL;
    return vm_map_ram(vma, 0, r->dma_addr, r->dma_bytes);
}

static uint32_t pcm_poll(struct file *f)
{
    struct snd_pcm_substream *s = f->priv;
    struct snd_pcm_runtime *r = &s->runtime;
    int play = s->stream == SNDRV_PCM_STREAM_PLAYBACK;
    uint32_t ok = play ? POLLOUT | POLLWRNORM : POLLIN | POLLRDNORM;
    unsigned long flags = irq_save();
    update(s);
    snd_pcm_uframes_t a = r->state == SNDRV_PCM_STATE_OPEN ? 0 : avail(s);
    int state = r->state;
    irq_restore(flags);
    switch (state) {
    case SNDRV_PCM_STATE_RUNNING:
    case SNDRV_PCM_STATE_PREPARED:
    case SNDRV_PCM_STATE_PAUSED:
        return a >= r->avail_min ? ok : 0;
    case SNDRV_PCM_STATE_DRAINING:
        return play ? 0 : a ? ok : ok | POLLERR;
    }
    return ok | POLLERR;
}

const struct file_ops snd_pcm_fops = {
    .open = pcm_open, .release = pcm_release, .ioctl = pcm_ioctl,
    .mmap = pcm_mmap, .poll = pcm_poll,
};

/* --- for drivers ---------------------------------------------------------------- */

int snd_pcm_new(struct snd_card *card, const char *id, struct snd_pcm **out)
{
    static struct snd_pcm pcms[SNDRV_CARDS];
    struct snd_pcm *pcm = &pcms[card->number];
    memset(pcm, 0, sizeof *pcm);
    pcm->card = card;
    strlcpy(pcm->id, id, sizeof pcm->id);
    strlcpy(pcm->name, id, sizeof pcm->name);
    for (int k = 0; k < 2; k++) {
        pcm->streams[k].pcm = pcm;
        pcm->streams[k].stream = k;
    }
    card->pcm = pcm;
    *out = pcm;
    return 0;
}

void snd_pcm_set_ops(struct snd_pcm *pcm, int stream, const struct snd_pcm_ops *ops,
                     const struct snd_pcm_hardware *hw, void *private_data)
{
    pcm->streams[stream].ops = ops;
    pcm->streams[stream].hw = *hw;
    pcm->streams[stream].private_data = private_data;
    pcm->has[stream] = 1;
}

void snd_pcm_set_buffer(struct snd_pcm *pcm, int stream, void *area, uint32_t phys,
                        unsigned int bytes)
{
    struct snd_pcm_runtime *r = &pcm->streams[stream].runtime;
    r->dma_area = area;
    r->dma_addr = phys;
    r->dma_bytes = bytes;
}
