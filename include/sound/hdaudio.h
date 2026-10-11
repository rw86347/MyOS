/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * hdaudio.h — the HD Audio bus and its codecs, as Linux shapes them
 * (include/sound/hdaudio.h): a controller is a `hdac_bus`, each codec on
 * its link a `hdac_device`, and codec drivers talk to them with
 * snd_hdac_codec_read/write, which take the same arguments as Linux's.
 * Ours, written from the HD Audio spec (docs/SOUND-PLAN.md, option A).
 */
#ifndef SOUND_HDAUDIO_H
#define SOUND_HDAUDIO_H

#include <linux/mutex.h>
#include <stdint.h>
#include <sound/hda_verbs.h>

typedef uint16_t hda_nid_t;             /* a node (widget) number */

#define HDA_MAX_CODECS 8                /* codec addresses on one link: 0..7 here
                                           (the spec allows 15) */

struct hdac_bus;

/* What a controller does for the bus (Linux's struct hdac_bus_ops). */
struct hdac_bus_ops {
    int (*command)(struct hdac_bus *bus, unsigned int cmd);    /* send a verb */
    int (*get_response)(struct hdac_bus *bus, unsigned int addr, unsigned int *res);
};

/* One codec on the link. */
struct hdac_device {
    struct hdac_bus *bus;
    unsigned int addr;                  /* its address on the link (0-14) */
    uint32_t vendor_id;                 /* vendor << 16 | device: 10ec0233 */
    uint32_t subsystem_id;              /* the machine's: 1462 12ea on the MSI */
    uint32_t revision_id;
    hda_nid_t afg;                      /* the audio function group node, 0: none */
    hda_nid_t mfg;                      /* a modem function group, 0: none */
    hda_nid_t start_nid;                /* the audio function's widgets: */
    int num_nodes;                      /*   start_nid .. start_nid + num_nodes - 1 */
};

/* A controller's link (Linux's struct hdac_bus). */
struct hdac_bus {
    const struct hdac_bus_ops *ops;
    void *priv;                         /* the controller driver's */
    unsigned int codec_mask;            /* which addresses answered at reset */
    struct hdac_device *codecs[HDA_MAX_CODECS];
    struct mutex cmd_lock;              /* one verb and its answer at a time */
};

/* The 32-bit command for a verb: a 12-bit verb with an 8-bit payload, or
   a 4-bit one (verb 0x2-0x5 << 8) with a 16-bit payload. */
unsigned int snd_hdac_make_cmd(struct hdac_device *codec, hda_nid_t nid,
                               unsigned int verb, unsigned int parm);

/* Send a verb and wait for the answer: the answer, or -1. (flags: 0.) */
int snd_hdac_codec_read(struct hdac_device *hdac, hda_nid_t nid, int flags,
                        unsigned int verb, unsigned int parm);
/* Send a verb that answers nothing useful: 0, or -1. */
int snd_hdac_codec_write(struct hdac_device *hdac, hda_nid_t nid, int flags,
                         unsigned int verb, unsigned int parm);

/* A parameter of a node (AC_PAR_*), or -1. */
int snd_hdac_read_parm(struct hdac_device *codec, hda_nid_t nid, int parm);

/* The nodes under nid: how many, and the first one in *start_id. */
int snd_hdac_get_sub_nodes(struct hdac_device *codec, hda_nid_t nid, hda_nid_t *start_id);

/* Find the codecs the reset found (bus->codec_mask), read what they are,
   and print it (for a new machine, the facts its driver will need). */
void snd_hdac_bus_scan(struct hdac_bus *bus, const char *who);

/* Find the HD Audio controllers (PCI class 0403), bring them up, and scan
   their codecs (drivers/sound/hda_intel.c). At boot, after pci_init. */
void hda_intel_init(void);

/* The generic codec driver (drivers/sound/hda_codec.c). */
struct hda_gen;
struct snd_card;
struct snd_pcm_hardware;
struct hda_gen *hda_gen_probe(struct hdac_device *codec);  /* NULL: no analog outputs */
const char *hda_gen_chip(struct hda_gen *g);                /* "ALC256" */
int hda_gen_has_capture(struct hda_gen *g);
void hda_gen_pcm_caps(struct hda_gen *g, int stream, struct snd_pcm_hardware *hw);
int hda_gen_add_controls(struct hda_gen *g, struct snd_card *card);
void hda_gen_stream(struct hda_gen *g, int stream, unsigned int tag, unsigned int fmt);
void hda_gen_unsol(struct hda_gen *g, unsigned int res);   /* a jack changed */
unsigned int hda_format(unsigned int rate, unsigned int bits, unsigned int channels,
                        unsigned int msbits);

#endif
