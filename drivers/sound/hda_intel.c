/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * hda_intel.c — HD Audio controllers ("Azalia"): the MSI's Tiger Lake-H
 * 8086:43c8, QEMU's ich9-intel-hda, and any other PCI class 0403 device.
 * Ours, from Intel's High Definition Audio Specification 1.0a; the register
 * names are Linux's (AZX_REG_*), so it reads like snd-hda-intel.
 * docs/SOUND-PLAN.md.
 *
 * The controller sits between memory and a serial link to the codecs. Two
 * rings in RAM carry the conversation with them, much like xHCI's command
 * and event rings: we put 32-bit commands ("verbs") in the CORB (Command
 * Outbound Ring Buffer) and move its write pointer; the controller sends
 * them down the link and writes each codec's 64-bit answer into the RIRB
 * (Response Inbound Ring Buffer).
 *
 * Sound moves through DMA "streams": a stream descriptor (registers) points
 * at a buffer descriptor list (BDL) in RAM — one entry per period of our
 * sound buffer, each asking for an interrupt when done — and the
 * controller plays the buffer round and round, sending the samples down
 * the link tagged with the stream's number. The codec's converter (DAC)
 * set to that number plays them. The controller also writes each stream's
 * position into a page of RAM (the DMA position buffer), so asking where
 * playback is costs a memory read, not a register read.
 *
 * Codec commands at boot are waited for by polling the RIRB, with a time
 * limit. After that, an answer nobody asked for — "headphones went in" —
 * raises the RIRB interrupt, which only wakes the jack thread: it reads
 * the answer and asks the codec driver to switch the outputs.
 */
#include <errno.h>
#include <interrupts.h>
#include <kprintf.h>
#include <memlayout.h>
#include <pci.h>
#include <pmm.h>
#include <sound/core.h>
#include <sound/hdaudio.h>
#include <string.h>
#include <thread.h>
#include <timer.h>

#define AZX_MAX 2                       /* controllers: the PCH's, a GPU's HDMI */
#define BUF_PAGES   64                  /* 256 KiB per stream: 1.3 s at 48 kHz stereo */
#define MAX_PERIODS 32                  /* Linux's AZX_MAX_FRAG */

/* Controller registers (Linux's names, include/sound/hda_register.h) */
#define AZX_REG_GCAP        0x00        /* 16: stream counts */
#define AZX_REG_VMIN        0x02
#define AZX_REG_VMAJ        0x03
#define AZX_REG_GCTL        0x08
#define   AZX_GCTL_RESET    (1u << 0)   /* 0: in reset */
#define   AZX_GCTL_UNSOL    (1u << 8)   /* accept unsolicited responses */
#define AZX_REG_STATESTS    0x0e        /* 16: which codecs spoke after reset */
#define AZX_REG_INTCTL      0x20
#define   AZX_INT_GLOBAL_EN (1u << 31)
#define   AZX_INT_CTRL_EN   (1u << 30)  /* the controller's own: the RIRB */
#define AZX_REG_INTSTS      0x24
#define AZX_REG_CORBLBASE   0x40
#define AZX_REG_CORBUBASE   0x44
#define AZX_REG_CORBWP      0x48        /* 16: the last command written */
#define AZX_REG_CORBRP      0x4a        /* 16: the last command sent */
#define   AZX_CORBRP_RST    (1u << 15)
#define AZX_REG_CORBCTL     0x4c
#define   AZX_CORBCTL_RUN   (1u << 1)
#define AZX_REG_CORBSIZE    0x4e
#define AZX_REG_RIRBLBASE   0x50
#define AZX_REG_RIRBUBASE   0x54
#define AZX_REG_RIRBWP      0x58        /* 16: the last answer written */
#define   AZX_RIRBWP_RST    (1u << 15)
#define AZX_REG_RINTCNT     0x5a
#define AZX_REG_RIRBCTL     0x5c
#define   AZX_RBCTL_IRQ_EN  (1u << 0)
#define   AZX_RBCTL_DMA_EN  (1u << 1)
#define AZX_REG_RIRBSTS     0x5d
#define   AZX_RBSTS_IRQ     (1u << 0)
#define   AZX_RBSTS_OVERRUN (1u << 2)
#define AZX_REG_RIRBSIZE    0x5e
#define AZX_REG_DPLBASE     0x70        /* the DMA position buffer; bit 0: on */
#define AZX_REG_DPUBASE     0x74

/* Stream descriptor n: inputs first, then outputs, 0x20 bytes each */
#define SD(n)               (0x80 + 0x20 * (n))
#define SD_CTL              0x00        /* 24 bits */
#define   SD_CTL_SRST       (1u << 0)   /* stream reset */
#define   SD_CTL_RUN        (1u << 1)
#define   SD_INT_COMPLETE   (1u << 2)   /* interrupt at the end of a BDL entry */
#define   SD_INT_FIFO_ERR   (1u << 3)
#define   SD_INT_DESC_ERR   (1u << 4)
#define   SD_CTL_TAG_SHIFT  20          /* bits 23:20: the stream number */
#define SD_STS              0x03
#define SD_LPIB             0x04        /* where it is, in bytes */
#define SD_CBL              0x08        /* the buffer's length */
#define SD_LVI              0x0c        /* the last BDL entry */
#define SD_FORMAT           0x12
#define SD_BDLPL            0x18
#define SD_BDLPU            0x1c
#define SD_INT_MASK         (SD_INT_COMPLETE | SD_INT_FIFO_ERR | SD_INT_DESC_ERR)

/* Intel PCI configuration (Linux: AZX_PCIREG_TCSEL, INTEL_SCH_HDA_DEVC) */
#define AZX_PCIREG_TCSEL        0x44    /* traffic class: bits 2:0 must be 0 */
#define INTEL_SCH_HDA_DEVC      0x78
#define   INTEL_DEVC_NOSNOOP    (1u << 11)
#define INTEL_HDA_CGCTL         0x48    /* Skylake and later: clock gating */
#define   INTEL_HDA_CGCTL_MISCBDCGE (1u << 6)

/* Intel's Skylake-and-later controllers (Linux: AZX_DRIVER_SKL), the MSI's
   Tiger Lake-H 43c8 among them. */
static const uint16_t intel_skl[] = {
    0xa170, 0x9d70, 0xa171, 0x9d71, 0xa2f0, 0x9dc8, 0xa348, 0x02c8, 0x06c8, 0xa3f0,
    0xf0c8, 0x34c8, 0x3dc8, 0x38c8, 0x4dc8, 0xa0c8, 0x43c8, 0xf1c8, 0x4b55, 0x4b58,
    0x490d, 0x4f90, 0x4f91, 0x4f92, 0x7ad0, 0x51c8, 0x51c9, 0x51cd, 0x51cc, 0x54c8,
    0x51ca, 0x7a50,
};

static int is_skl(struct pci_dev *pci)
{
    for (unsigned int k = 0; k < sizeof intel_skl / sizeof *intel_skl; k++)
        if (pci->vendor == 0x8086 && pci->device == intel_skl[k])
            return 1;
    return 0;
}

#define RIRB_EX_UNSOL   0x10            /* an answer nobody asked for (a jack) */
#define RIRB_EX_ADDR    0x0f
#define UNSOL_QUEUE     16

struct azx;

/* One DMA stream: playback (an output descriptor) or capture (an input one). */
struct azx_stream {
    struct azx *chip;
    int index;                          /* stream descriptor number */
    unsigned int tag;                   /* the number on the link, 1..15 */
    struct snd_pcm_substream *substream;
    uint32_t *bdl;                      /* up to 256 entries of 16 bytes */
    uint32_t bdl_phys;
    uint8_t *buf;
    uint32_t buf_phys;
    unsigned int fmt;
    int running;
};

struct azx {
    struct pci_dev *pci;
    volatile uint8_t *regs;
    struct hdac_bus bus;
    uint32_t *corb;                     /* 256 commands */
    uint32_t *rirb;                     /* 256 answers: (response, extra) pairs */
    volatile uint32_t *posbuf;          /* the controller writes stream positions here */
    uint16_t corb_size, rirb_size;
    uint16_t corb_wp, rirb_rp;
    uint8_t warned;
    const char *name;                   /* "00:1f.3" */
    int irq_ok;
    struct azx_stream play, rec;
    struct hda_gen *gen;
    struct hdac_device *codec;          /* the one gen drives */
    struct snd_card *card;
    /* answers nobody asked for, for the jack thread */
    unsigned int unsol[UNSOL_QUEUE], unsol_n;
    struct wait_queue jack_wq;
    int jack_pending;
};

static struct azx chips[AZX_MAX];
static int nchips;

static uint8_t  rd8(struct azx *c, uint32_t r)  { return *(volatile uint8_t *)(c->regs + r); }
static uint16_t rd16(struct azx *c, uint32_t r) { return *(volatile uint16_t *)(c->regs + r); }
static uint32_t rd32(struct azx *c, uint32_t r) { return *(volatile uint32_t *)(c->regs + r); }
static void wr8(struct azx *c, uint32_t r, uint8_t v)   { *(volatile uint8_t *)(c->regs + r) = v; }
static void wr16(struct azx *c, uint32_t r, uint16_t v) { *(volatile uint16_t *)(c->regs + r) = v; }
static void wr32(struct azx *c, uint32_t r, uint32_t v) { *(volatile uint32_t *)(c->regs + r) = v; }

static void azx_sleep(uint32_t ms)
{
    struct wait_queue q = { 0 };
    unsigned long flags = irq_save();
    wq_sleep_until(&q, timer_ms() + ms);
    irq_restore(flags);
}

/* Wait until (register & mask) == want, for at most ms. */
static int wait16(struct azx *c, uint32_t r, uint16_t mask, uint16_t want, uint32_t ms)
{
    uint32_t end = timer_ms() + ms;
    while ((rd16(c, r) & mask) != want) {
        if ((int32_t)(timer_ms() - end) > 0)
            return -1;
        azx_sleep(1);
    }
    return 0;
}

static int wait32(struct azx *c, uint32_t r, uint32_t mask, uint32_t want, uint32_t ms)
{
    uint32_t end = timer_ms() + ms;
    while ((rd32(c, r) & mask) != want) {
        if ((int32_t)(timer_ms() - end) > 0)
            return -1;
        azx_sleep(1);
    }
    return 0;
}

static int wait8(struct azx *c, uint32_t r, uint8_t mask, uint8_t want, uint32_t ms)
{
    uint32_t end = timer_ms() + ms;
    while ((rd8(c, r) & mask) != want) {
        if ((int32_t)(timer_ms() - end) > 0)
            return -1;
        azx_sleep(1);
    }
    return 0;
}

/* --- the conversation with the codecs (callers hold bus.cmd_lock) ------------------ */

static int azx_command(struct hdac_bus *bus, unsigned int cmd)
{
    struct azx *c = bus->priv;
    uint16_t wp = (uint16_t)((c->corb_wp + 1) % c->corb_size);
    if (wp == (rd16(c, AZX_REG_CORBRP) & 0xff) % c->corb_size)
        return -1;                      /* full (we wait for each answer: can't happen) */
    c->corb[wp] = cmd;
    __asm__ volatile ("" ::: "memory");
    wr16(c, AZX_REG_CORBWP, wp);
    c->corb_wp = wp;
    return 0;
}

/* Take the RIRB's new entries: the first answer from `addr` (if want), and
   every unsolicited one into the jack queue. 1: got the answer. */
static int rirb_take(struct azx *c, int want, unsigned int addr, unsigned int *res)
{
    /* Clear the response-count status first: a controller may hold the
       CORB (QEMU's does) until it is cleared, which the interrupt handler
       does too, but at boot we poll with that interrupt off. */
    uint8_t sts = rd8(c, AZX_REG_RIRBSTS);
    if (sts & (AZX_RBSTS_IRQ | AZX_RBSTS_OVERRUN))
        wr8(c, AZX_REG_RIRBSTS, sts);
    uint16_t wp = (uint16_t)((rd16(c, AZX_REG_RIRBWP) & 0xff) % c->rirb_size);
    while (c->rirb_rp != wp) {
        c->rirb_rp = (uint16_t)((c->rirb_rp + 1) % c->rirb_size);
        uint32_t r = c->rirb[c->rirb_rp * 2], ex = c->rirb[c->rirb_rp * 2 + 1];
        if (ex & RIRB_EX_UNSOL) {
            unsigned long flags = irq_save();
            if (c->unsol_n < UNSOL_QUEUE)
                c->unsol[c->unsol_n++] = r;
            c->jack_pending = 1;
            wq_wake_all(&c->jack_wq);
            irq_restore(flags);
        } else if (want && (ex & RIRB_EX_ADDR) == addr) {
            *res = r;
            return 1;
        }
    }
    return 0;
}

/* The next answer from codec `addr`: microseconds normally, so a short
   spin, then 1 ms sleeps up to 50 ms. */
static int azx_get_response(struct hdac_bus *bus, unsigned int addr, unsigned int *res)
{
    struct azx *c = bus->priv;
    uint32_t end = 0;
    for (int spins = 0; ; spins++) {
        if (rirb_take(c, 1, addr, res))
            return 0;
        if (spins < 2000)
            continue;                   /* a codec answers within microseconds */
        if (!end)
            end = timer_ms() + 50;
        else if ((int32_t)(timer_ms() - end) > 0)
            break;
        azx_sleep(1);
    }
    if (!c->warned) {
        kprintf("hda: %s: codec %u did not answer\n", c->name, addr);
        c->warned = 1;
    }
    return -1;
}

static const struct hdac_bus_ops azx_ops = {
    .command = azx_command,
    .get_response = azx_get_response,
};

/* --- the interrupt ------------------------------------------------------------------ */

static void stream_irq(struct azx *c, struct azx_stream *st)
{
    uint8_t sts = rd8(c, SD(st->index) + SD_STS);
    wr8(c, SD(st->index) + SD_STS, sts & SD_INT_MASK);     /* write 1 to clear */
    if ((sts & SD_INT_COMPLETE) && st->substream && st->running)
        snd_pcm_period_elapsed(st->substream);
}

/* Minimal: clear, note, wake. No codec commands, no copying. */
static void azx_irq(struct regs *r)
{
    (void)r;
    for (int i = 0; i < nchips; i++) {
        struct azx *c = &chips[i];
        if (!c->irq_ok)
            continue;
        uint32_t sts = rd32(c, AZX_REG_INTSTS);
        if (!sts || sts == 0xffffffffu)
            continue;                   /* not this one */
        if (sts & (1u << c->play.index))
            stream_irq(c, &c->play);
        if (sts & (1u << c->rec.index))
            stream_irq(c, &c->rec);
        uint8_t rb = rd8(c, AZX_REG_RIRBSTS);
        if (rb & (AZX_RBSTS_IRQ | AZX_RBSTS_OVERRUN)) {
            wr8(c, AZX_REG_RIRBSTS, rb);
            c->jack_pending = 1;        /* maybe an unsolicited answer */
            wq_wake_all(&c->jack_wq);
        }
    }
}

/* Jacks: read what the RIRB has, then let the codec driver look. */
static void jack_thread(void *arg)
{
    struct azx *c = arg;
    for (;;) {
        unsigned long flags = irq_save();
        while (!c->jack_pending)
            wq_sleep(&c->jack_wq);
        c->jack_pending = 0;
        irq_restore(flags);
        mutex_lock(&c->bus.cmd_lock);
        rirb_take(c, 0, 0, NULL);
        mutex_unlock(&c->bus.cmd_lock);
        flags = irq_save();
        unsigned int n = c->unsol_n;
        c->unsol_n = 0;
        irq_restore(flags);
        if (n && c->gen)
            hda_gen_unsol(c->gen, 0);
    }
}

/* --- streams ------------------------------------------------------------------------ */

static int stream_reset(struct azx *c, struct azx_stream *st)
{
    uint32_t sd = SD(st->index);
    wr8(c, sd + SD_CTL, rd8(c, sd + SD_CTL) & ~(SD_CTL_RUN | SD_INT_MASK));
    wait8(c, sd + SD_CTL, SD_CTL_RUN, 0, 10);
    wr8(c, sd + SD_CTL, rd8(c, sd + SD_CTL) | SD_CTL_SRST);
    if (wait8(c, sd + SD_CTL, SD_CTL_SRST, SD_CTL_SRST, 10) < 0)
        return -1;
    wr8(c, sd + SD_CTL, rd8(c, sd + SD_CTL) & ~SD_CTL_SRST);
    return wait8(c, sd + SD_CTL, SD_CTL_SRST, 0, 10);
}

static struct azx_stream *stream_of(struct snd_pcm_substream *s)
{
    struct azx *c = s->private_data;
    return s->stream == SNDRV_PCM_STREAM_PLAYBACK ? &c->play : &c->rec;
}

/* Prepare: one BDL entry per period, each asking for an interrupt; the
   format to the controller and to the codec's converters. Sleeps. */
static int azx_prepare(struct snd_pcm_substream *s)
{
    struct azx *c = s->private_data;
    struct azx_stream *st = stream_of(s);
    struct snd_pcm_runtime *r = &s->runtime;
    uint32_t sd = SD(st->index), period = (uint32_t)(r->period_size * r->frame_bytes);
    if (stream_reset(c, st) < 0)
        return -EIO;
    for (unsigned int k = 0; k < r->periods; k++) {
        uint32_t *e = st->bdl + 4 * k;
        e[0] = st->buf_phys + k * period;
        e[1] = 0;
        e[2] = period;
        e[3] = 1;                       /* IOC: interrupt when done */
    }
    st->fmt = hda_format(r->rate, r->sample_bits, r->channels, s->hw.msbits);
    wr32(c, sd + SD_BDLPL, st->bdl_phys);
    wr32(c, sd + SD_BDLPU, 0);
    wr32(c, sd + SD_CBL, period * r->periods);
    wr16(c, sd + SD_LVI, (uint16_t)(r->periods - 1));
    wr16(c, sd + SD_FORMAT, (uint16_t)st->fmt);
    wr32(c, sd + SD_CTL, (rd32(c, sd + SD_CTL) & ~(0xfu << SD_CTL_TAG_SHIFT)) |
                         st->tag << SD_CTL_TAG_SHIFT);
    c->posbuf[st->index * 2] = 0;
    hda_gen_stream(c->gen, s->stream, st->tag, st->fmt);
    return 0;
}

static int azx_hw_free(struct snd_pcm_substream *s)
{
    struct azx *c = s->private_data;
    hda_gen_stream(c->gen, s->stream, 0, 0);
    stream_reset(c, stream_of(s));
    return 0;
}

/* Start and stop: register writes only (called with interrupts off). */
static int azx_trigger(struct snd_pcm_substream *s, int cmd)
{
    struct azx *c = s->private_data;
    struct azx_stream *st = stream_of(s);
    uint32_t sd = SD(st->index);
    uint8_t ctl = rd8(c, sd + SD_CTL);
    switch (cmd) {
    case SNDRV_PCM_TRIGGER_START:
    case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
        wr32(c, AZX_REG_INTCTL, rd32(c, AZX_REG_INTCTL) | 1u << st->index);
        st->running = 1;
        wr8(c, sd + SD_CTL, ctl | SD_CTL_RUN | SD_INT_MASK);
        return 0;
    case SNDRV_PCM_TRIGGER_STOP:
    case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
        wr8(c, sd + SD_CTL, ctl & ~(SD_CTL_RUN | SD_INT_MASK));
        wr8(c, sd + SD_STS, SD_INT_MASK);
        st->running = 0;
        wr32(c, AZX_REG_INTCTL, rd32(c, AZX_REG_INTCTL) & ~(1u << st->index));
        return 0;
    }
    return -EINVAL;
}

/* Where the DMA is. Playback: from the position buffer (a memory read).
   Capture: from the LPIB register — on Skylake and later Intel chips the
   position buffer runs ahead of what is really in memory for capture, so
   Linux reads LPIB there too. */
static snd_pcm_uframes_t azx_pointer(struct snd_pcm_substream *s)
{
    struct azx *c = s->private_data;
    struct azx_stream *st = stream_of(s);
    uint32_t pos = s->stream == SNDRV_PCM_STREAM_PLAYBACK ? c->posbuf[st->index * 2]
                                                         : rd32(c, SD(st->index) + SD_LPIB);
    uint32_t bytes = (uint32_t)(s->runtime.buffer_size * s->runtime.frame_bytes);
    if (pos >= bytes)
        pos = 0;
    return pos / s->runtime.frame_bytes;
}

static const struct snd_pcm_ops azx_pcm_ops = {
    .prepare = azx_prepare,
    .hw_free = azx_hw_free,
    .trigger = azx_trigger,
    .pointer = azx_pointer,
};

static int stream_init(struct azx *c, struct azx_stream *st, int index, unsigned int tag)
{
    st->chip = c;
    st->index = index;
    st->tag = tag;
    st->bdl_phys = pmm_alloc();
    st->buf_phys = pmm_alloc_run(BUF_PAGES);
    if (!st->bdl_phys || !st->buf_phys)
        return -1;
    st->bdl = P2V(st->bdl_phys);
    st->buf = P2V(st->buf_phys);
    memset(st->bdl, 0, PAGE_SIZE);
    memset(st->buf, 0, BUF_PAGES * PAGE_SIZE);
    return 0;
}

/* --- the card: what programs see -------------------------------------------------- */

static char *put(char *p, const char *s)
{
    while (*s)
        *p++ = *s++;
    *p = 0;
    return p;
}

static char *put_hex(char *p, uint32_t v, int digits)
{
    while (digits--)
        *p++ = "0123456789abcdef"[(v >> (4 * digits)) & 0xf];
    *p = 0;
    return p;
}

static void make_card(struct azx *c, int iss, int oss)
{
    struct snd_card *card;
    struct snd_pcm *pcm;
    if (!oss || snd_card_new(&card) < 0)
        return;
    c->card = card;
    card->private_data = c;
    const char *chip = hda_gen_chip(c->gen);
    struct hdac_device *codec = c->codec;
    /* Linux's names: alsa-lib picks its settings (cards/HDA-Intel.conf) by
       the driver name, PulseAudio its profiles by the card's. */
    put(card->driver, "HDA-Intel");
    put(card->id, c->pci->vendor == 0x8086 ? "PCH" : "Generic");
    put(card->shortname, c->pci->vendor == 0x8086 ? "HDA Intel PCH" : "HD-Audio Generic");
    char *p = put(put(card->longname, card->shortname), " at 0x");
    put_hex(p, (uint32_t)pci_bar_phys(c->pci, 0), 8);
    put(put(card->mixername, (codec->vendor_id >> 16) == 0x10ec ? "Realtek " : ""), chip);
    p = put_hex(put(card->components, "HDA:"), codec->vendor_id, 8);
    p = put_hex(put(p, ","), codec->subsystem_id, 8);
    put_hex(put(p, ","), codec->revision_id, 8);
    snd_pcm_new(card, chip, &pcm);
    put(put(pcm->name, chip), " Analog");
    put(pcm->id, pcm->name);

    struct snd_pcm_hardware hw = {
        .info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID | SNDRV_PCM_INFO_INTERLEAVED |
                SNDRV_PCM_INFO_BLOCK_TRANSFER | SNDRV_PCM_INFO_PAUSE,
        .channels_min = 2, .channels_max = 2,
        .buffer_bytes_max = BUF_PAGES * PAGE_SIZE,
        .period_bytes_min = 128, .period_bytes_max = BUF_PAGES * PAGE_SIZE / 2,
        .periods_min = 2, .periods_max = MAX_PERIODS,
        .step_bytes = 128,              /* the spec: BDL entries in 128-byte units */
    };
    if (stream_init(c, &c->play, iss, 1) == 0) {
        hda_gen_pcm_caps(c->gen, SNDRV_PCM_STREAM_PLAYBACK, &hw);
        snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &azx_pcm_ops, &hw, c);
        snd_pcm_set_buffer(pcm, SNDRV_PCM_STREAM_PLAYBACK, c->play.buf, c->play.buf_phys,
                           BUF_PAGES * PAGE_SIZE);
        c->play.substream = &pcm->streams[SNDRV_PCM_STREAM_PLAYBACK];
    }
    if (iss && hda_gen_has_capture(c->gen) && stream_init(c, &c->rec, 0, 2) == 0) {
        hda_gen_pcm_caps(c->gen, SNDRV_PCM_STREAM_CAPTURE, &hw);
        snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &azx_pcm_ops, &hw, c);
        snd_pcm_set_buffer(pcm, SNDRV_PCM_STREAM_CAPTURE, c->rec.buf, c->rec.buf_phys,
                           BUF_PAGES * PAGE_SIZE);
        c->rec.substream = &pcm->streams[SNDRV_PCM_STREAM_CAPTURE];
    }
    hda_gen_add_controls(c->gen, card);
    snd_card_register(card);
}

/* --- bringing a controller up ------------------------------------------------------ */

/* The ring size field: bits 7:4 say what the controller can do (bit 6: 256
   entries, bit 5: 16, bit 4: 2); bits 1:0 choose. The biggest it allows. */
static uint16_t ring_size(struct azx *c, uint32_t reg)
{
    uint8_t cap = rd8(c, reg) >> 4;
    uint8_t code = cap & 4 ? 2 : cap & 2 ? 1 : 0;
    wr8(c, reg, (uint8_t)((rd8(c, reg) & ~3u) | code));
    return code == 2 ? 256 : code == 1 ? 16 : 2;
}

static int azx_reset(struct azx *c)
{
    /* Into reset, then out of it: the link restarts and every codec that is
       there reports itself in STATESTS. The spec gives codecs 521 us after
       reset to do so; Linux waits a millisecond, so do we (and a bit). */
    wr32(c, AZX_REG_GCTL, rd32(c, AZX_REG_GCTL) & ~AZX_GCTL_RESET);
    if (wait32(c, AZX_REG_GCTL, AZX_GCTL_RESET, 0, 100) < 0)
        return -1;
    azx_sleep(1);
    wr32(c, AZX_REG_GCTL, rd32(c, AZX_REG_GCTL) | AZX_GCTL_RESET);
    if (wait32(c, AZX_REG_GCTL, AZX_GCTL_RESET, AZX_GCTL_RESET, 100) < 0)
        return -1;
    azx_sleep(2);
    return 0;
}

static int azx_rings(struct azx *c)
{
    /* one page: the CORB (1 KiB), the RIRB at 2 KiB; another: positions */
    uint32_t phys = pmm_alloc(), pos = pmm_alloc();
    if (!phys || !pos)
        return -1;
    memset(P2V(phys), 0, PAGE_SIZE);
    memset(P2V(pos), 0, PAGE_SIZE);
    c->corb = P2V(phys);
    c->rirb = (uint32_t *)((uint8_t *)P2V(phys) + 2048);
    c->posbuf = P2V(pos);

    wr8(c, AZX_REG_CORBCTL, 0);         /* stopped while it is set up */
    wr8(c, AZX_REG_RIRBCTL, 0);
    c->corb_size = ring_size(c, AZX_REG_CORBSIZE);
    c->rirb_size = ring_size(c, AZX_REG_RIRBSIZE);

    wr32(c, AZX_REG_CORBLBASE, phys);
    wr32(c, AZX_REG_CORBUBASE, 0);
    wr16(c, AZX_REG_CORBWP, 0);
    c->corb_wp = 0;
    /* Reset the read pointer: set the bit, wait for it to read back, clear
       it, wait again (the spec's handshake; some controllers skip the
       read-back, so a timeout here is not fatal). */
    wr16(c, AZX_REG_CORBRP, AZX_CORBRP_RST);
    wait16(c, AZX_REG_CORBRP, AZX_CORBRP_RST, AZX_CORBRP_RST, 10);
    wr16(c, AZX_REG_CORBRP, 0);
    wait16(c, AZX_REG_CORBRP, AZX_CORBRP_RST, 0, 10);
    wr8(c, AZX_REG_CORBCTL, AZX_CORBCTL_RUN);

    wr32(c, AZX_REG_RIRBLBASE, phys + 2048);
    wr32(c, AZX_REG_RIRBUBASE, 0);
    wr16(c, AZX_REG_RIRBWP, AZX_RIRBWP_RST);
    c->rirb_rp = 0;
    wr16(c, AZX_REG_RINTCNT, 1);
    wr8(c, AZX_REG_RIRBSTS, rd8(c, AZX_REG_RIRBSTS));  /* clear old status */
    /* The RIRB's interrupt is on from the start, as Linux has it: some
       controllers (QEMU's) only count responses, and stop taking commands
       at RINTCNT, while it is. The CPU hears nothing until INTCTL says so
       (after the probe); until then rirb_take clears the status itself. */
    wr8(c, AZX_REG_RIRBCTL, AZX_RBCTL_DMA_EN | AZX_RBCTL_IRQ_EN);

    wr32(c, AZX_REG_DPLBASE, pos | 1);
    wr32(c, AZX_REG_DPUBASE, 0);
    return 0;
}

static void azx_probe(struct pci_dev *pci)
{
    struct azx *c = &chips[nchips];
    memset(c, 0, sizeof *c);
    c->pci = pci;
    c->name = pci->dev.name + 5;        /* PCI's "0000:00:1f.3", without the domain */
    mutex_init(&c->bus.cmd_lock);
    phys_addr_t bar = pci_bar_phys(pci, 0);
    c->regs = bar ? pci_map_mmio(bar, 0x4000) : NULL;
    if (!c->regs) {
        kprintf("hda: %s: cannot map its registers\n", c->name);
        return;
    }
    pci_write_config(pci, 4, pci_read_config(pci, 4) | 0x6);   /* memory + bus master */
    if (pci->vendor == 0x8086) {
        /* As Linux does for Intel's controllers (azx_init_pci): traffic
           class 0 (else some codecs play static), and snooped DMA, so the
           controller sees what the CPU's caches hold. */
        pci_write_config(pci, AZX_PCIREG_TCSEL, pci_read_config(pci, AZX_PCIREG_TCSEL) & ~7u);
        pci_write_config(pci, INTEL_SCH_HDA_DEVC,
                         pci_read_config(pci, INTEL_SCH_HDA_DEVC) & ~INTEL_DEVC_NOSNOOP);
    }
    uint16_t gcap = rd16(c, AZX_REG_GCAP);
    int iss = (gcap >> 8) & 0xf, oss = (gcap >> 12) & 0xf;
    wr32(c, AZX_REG_INTCTL, 0);         /* no interrupts until we are ready */
    /* Skylake and later: no dynamic clock gating while the link resets,
       or the codecs may not be found (Linux does the same). */
    int skl = is_skl(pci);
    if (skl)
        pci_write_config(pci, INTEL_HDA_CGCTL,
                         pci_read_config(pci, INTEL_HDA_CGCTL) & ~INTEL_HDA_CGCTL_MISCBDCGE);
    int bad = azx_reset(c) < 0 || azx_rings(c) < 0;
    if (skl)
        pci_write_config(pci, INTEL_HDA_CGCTL,
                         pci_read_config(pci, INTEL_HDA_CGCTL) | INTEL_HDA_CGCTL_MISCBDCGE);
    if (bad) {
        kprintf("hda: %s: the controller did not come out of reset\n", c->name);
        return;
    }
    wr32(c, AZX_REG_GCTL, rd32(c, AZX_REG_GCTL) | AZX_GCTL_UNSOL);
    c->bus.ops = &azx_ops;
    c->bus.priv = c;
    c->bus.codec_mask = rd16(c, AZX_REG_STATESTS) & 0x7fff;
    wr16(c, AZX_REG_STATESTS, (uint16_t)c->bus.codec_mask);    /* write 1 to clear */
    uint32_t cls = pci->class_rev >> 8;
    klog("hda: %s %04x:%04x, class %06lx (%s), HD Audio %u.%u, %u out / %u in streams, "
            "codecs %02x\n", c->name, pci->vendor, pci->device, (unsigned long)cls,
            (cls & 0xff) == 0 ? "HD Audio" : "HD Audio with a DSP", rd8(c, AZX_REG_VMAJ),
            rd8(c, AZX_REG_VMIN), oss, iss, c->bus.codec_mask);
    nchips++;
    snd_hdac_bus_scan(&c->bus, c->name);

    /* The first codec with analog outputs is the sound card. */
    for (int a = 0; a < HDA_MAX_CODECS && !c->gen; a++)
        if (c->bus.codecs[a] && (c->gen = hda_gen_probe(c->bus.codecs[a])))
            c->codec = c->bus.codecs[a];
    if (!c->gen)
        return;
    if (pci_alloc_irq_vectors(pci, 1, 1, PCI_IRQ_MSI | PCI_IRQ_INTX) < 0) {
        kprintf("hda: %s: no interrupt: no sound\n", c->name);
        return;
    }
    irq_install(pci_irq_vector(pci, 0), azx_irq);
    c->irq_ok = 1;
    thread_create("hda-jack", jack_thread, c);
    wr32(c, AZX_REG_INTCTL, AZX_INT_GLOBAL_EN | AZX_INT_CTRL_EN);
    make_card(c, iss, oss);
}

void hda_intel_init(void)
{
    for (int i = 0; i < pci_count() && nchips < AZX_MAX; i++) {
        struct pci_dev *p = pci_dev_at(i);
        if (p->class_rev >> 16 == 0x0403)       /* multimedia: HD Audio */
            azx_probe(p);
    }
}
