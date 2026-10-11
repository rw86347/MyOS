/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * core.h — the sound core, as a driver sees it: Linux's ALSA driver API in
 * a small form. A driver makes a card (snd_card_new), gives it one PCM
 * device with a playback and a capture stream (snd_pcm_new) and mixer
 * controls (snd_ctl_new1 + snd_ctl_add), then registers it: /dev/snd/
 * controlC0, pcmC0D0p and pcmC0D0c appear. Its interrupt handler calls
 * snd_pcm_period_elapsed at the end of each period. The core does the rest
 * of ALSA: hw_params negotiation, the stream states, read/write/mmap/poll.
 * docs/SOUND-PLAN.md.
 */
#ifndef SOUND_CORE_H
#define SOUND_CORE_H

#include <sound/asound.h>
#include <stddef.h>
#include <stdint.h>
#include <thread.h>

#define SNDRV_CARDS         2
#define SND_CTL_MAX         32              /* controls per card */
#define SND_RATES_MAX       8

struct snd_card;
struct snd_pcm_substream;
struct snd_kcontrol;

/* --- PCM -------------------------------------------------------------------- */

#define SNDRV_PCM_TRIGGER_STOP          0
#define SNDRV_PCM_TRIGGER_START         1
#define SNDRV_PCM_TRIGGER_PAUSE_PUSH    3
#define SNDRV_PCM_TRIGGER_PAUSE_RELEASE 4

/* What the hardware can do (Linux: struct snd_pcm_hardware). */
struct snd_pcm_hardware {
    unsigned int info;                      /* SNDRV_PCM_INFO_* */
    uint64_t formats;                       /* 1 << SNDRV_PCM_FORMAT_* */
    unsigned int rates[SND_RATES_MAX];      /* the rates it plays, ascending */
    unsigned int nrates;
    unsigned int channels_min, channels_max;
    unsigned int buffer_bytes_max;
    unsigned int period_bytes_min, period_bytes_max;
    unsigned int periods_min, periods_max;
    unsigned int step_bytes;                /* periods and buffers: a multiple
                                               (HD Audio: 128) */
    unsigned int msbits;                    /* bits used in a 32-bit sample (24) */
};

struct snd_pcm_ops {
    int (*open)(struct snd_pcm_substream *s);
    int (*close)(struct snd_pcm_substream *s);
    int (*hw_params)(struct snd_pcm_substream *s);     /* runtime is filled in */
    int (*hw_free)(struct snd_pcm_substream *s);
    int (*prepare)(struct snd_pcm_substream *s);
    int (*trigger)(struct snd_pcm_substream *s, int cmd);   /* interrupts off */
    snd_pcm_uframes_t (*pointer)(struct snd_pcm_substream *s); /* frame in the buffer */
};

/* One open stream's settings and position (Linux: struct snd_pcm_runtime). */
struct snd_pcm_runtime {
    int state;
    unsigned int access, format, channels, rate;
    unsigned int sample_bits, frame_bytes;
    snd_pcm_uframes_t period_size, buffer_size;
    unsigned int periods;
    /* sw_params */
    int tstamp_mode;
    unsigned int tstamp_type;
    snd_pcm_uframes_t avail_min, start_threshold, stop_threshold, boundary;
    /* where the hardware and the program are, both counted modulo boundary */
    snd_pcm_uframes_t hw_ptr, appl_ptr, hw_ptr_base, avail_max;
    struct snd_timespec trigger_tstamp;
    /* the sound itself: the driver's buffer, mapped by programs */
    uint8_t *dma_area;
    uint32_t dma_addr;
    unsigned int dma_bytes;
    struct wait_queue sleep;                /* writers, readers and drain wait here */
};

struct snd_pcm_substream {
    struct snd_pcm *pcm;
    int stream;                             /* SNDRV_PCM_STREAM_* */
    const struct snd_pcm_ops *ops;
    struct snd_pcm_hardware hw;
    struct snd_pcm_runtime runtime;
    int opened;
    void *private_data;                     /* the driver's */
};

struct snd_pcm {
    struct snd_card *card;
    char id[64], name[80];
    struct snd_pcm_substream streams[2];    /* [SNDRV_PCM_STREAM_*] */
    int has[2];
};

/* The PCM device of `card` (device 0); the driver fills streams[] in. */
int snd_pcm_new(struct snd_card *card, const char *id, struct snd_pcm **pcm);
void snd_pcm_set_ops(struct snd_pcm *pcm, int stream, const struct snd_pcm_ops *ops,
                     const struct snd_pcm_hardware *hw, void *private_data);
/* The driver's buffer for one stream: programs map it, the hardware reads it. */
void snd_pcm_set_buffer(struct snd_pcm *pcm, int stream, void *area, uint32_t phys,
                        unsigned int bytes);
/* From the interrupt handler: a period has been played or recorded. */
void snd_pcm_period_elapsed(struct snd_pcm_substream *s);

/* --- controls ------------------------------------------------------------------- */

struct snd_kcontrol_new {
    int iface;                              /* SNDRV_CTL_ELEM_IFACE_* */
    const char *name;                       /* "Master Playback Volume" */
    unsigned int index, access;
    int (*info)(struct snd_kcontrol *k, struct snd_ctl_elem_info *uinfo);
    int (*get)(struct snd_kcontrol *k, struct snd_ctl_elem_value *v);
    int (*put)(struct snd_kcontrol *k, struct snd_ctl_elem_value *v);  /* 1: changed */
    unsigned long private_value;
};

struct snd_kcontrol {
    struct snd_ctl_elem_id id;
    const struct snd_kcontrol_new *ops;
    unsigned int access;
    const unsigned int *tlv;                /* type, length, values: dB range */
    unsigned long private_value;
    void *private_data;
};

struct snd_kcontrol *snd_ctl_new1(const struct snd_kcontrol_new *n, void *private_data);
int snd_ctl_add(struct snd_card *card, struct snd_kcontrol *k);
/* A control's value changed (a jack, a volume key): tell the listeners. */
void snd_ctl_notify(struct snd_card *card, unsigned int mask, struct snd_ctl_elem_id *id);

/* --- the card -------------------------------------------------------------------- */

struct snd_card {
    int number;
    char id[16], driver[16], shortname[32], longname[80], mixername[80], components[128];
    void *private_data;
    struct snd_pcm *pcm;
    struct snd_kcontrol *controls[SND_CTL_MAX];
    int ncontrols;
    struct ctl_file *listeners;             /* open control devices (events) */
    char node_names[3][12];                 /* the /dev/snd names: the VFS keeps
                                               a pointer, not a copy */
};

int snd_card_new(struct snd_card **card);
int snd_card_register(struct snd_card *card);  /* makes the /dev/snd nodes */

/* fs/dev.c: the file operations of /dev/snd/<minor> (major 116), or NULL */
const struct file_ops *snd_file_ops(unsigned int minor);

/* core.c and pcm.c, for each other */
struct snd_card *snd_card_get(int number);
extern const struct file_ops snd_pcm_fops;
int snd_pcm_info_fill(struct snd_pcm_substream *s, struct snd_pcm_info *info);

#endif
