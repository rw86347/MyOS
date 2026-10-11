/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * asound.h — the ALSA interface programs see: /dev/snd/controlC0 and
 * /dev/snd/pcmC0D0p (playback) / pcmC0D0c (capture), Linux's ioctl numbers
 * and structure layouts (include/uapi/sound/asound.h, protocol 2.0.18), so
 * alsa-lib, aplay, PulseAudio and browsers run unchanged. Our own header
 * with the same layouts, as for every other system call; only what we
 * implement. tools/check-asound.sh compiles Linux's header and this one
 * for both CPUs and compares every size, offset and ioctl number.
 *
 * Time: on x86_64 a timespec is two longs. On i386, musl's time_t is 64
 * bits (time64), and alsa-lib built against musl lays its timestamps out
 * that way, so ours do too: 8 bytes of seconds, 4 of nanoseconds, 4 of
 * padding, 4-byte aligned like every i386 long long.
 */
#ifndef SOUND_ASOUND_H
#define SOUND_ASOUND_H

#include <stdint.h>

#define SNDRV_PROTOCOL_VERSION(major, minor, sub) (((major) << 16) | ((minor) << 8) | (sub))
#define SNDRV_PCM_VERSION   SNDRV_PROTOCOL_VERSION(2, 0, 18)
#define SNDRV_CTL_VERSION   SNDRV_PROTOCOL_VERSION(2, 0, 9)

/* ioctl numbers, Linux's _IOC: direction (1 write, 2 read), size, type, number */
#define SND_IOC(dir, type, nr, size) \
    ((uint32_t)(((dir) << 30) | ((uint32_t)(size) << 16) | ((type) << 8) | (nr)))
#define SND_IO(t, nr)         SND_IOC(0, t, nr, 0)
#define SND_IOR(t, nr, ty)    SND_IOC(2, t, nr, sizeof(ty))
#define SND_IOW(t, nr, ty)    SND_IOC(1, t, nr, sizeof(ty))
#define SND_IOWR(t, nr, ty)   SND_IOC(3, t, nr, sizeof(ty))

typedef unsigned long snd_pcm_uframes_t;
typedef long snd_pcm_sframes_t;

#ifdef ARCH_X86_64
struct snd_timespec { int64_t tv_sec; long tv_nsec; };
#else
struct snd_timespec { int64_t tv_sec; int32_t tv_nsec, pad; };
#endif

/* --- PCM -------------------------------------------------------------------- */

#define SNDRV_PCM_STREAM_PLAYBACK   0
#define SNDRV_PCM_STREAM_CAPTURE    1

#define SNDRV_PCM_ACCESS_MMAP_INTERLEAVED   0
#define SNDRV_PCM_ACCESS_RW_INTERLEAVED     3

#define SNDRV_PCM_FORMAT_S16_LE     2
#define SNDRV_PCM_FORMAT_S32_LE     10
#define SNDRV_PCM_SUBFORMAT_STD     0

#define SNDRV_PCM_STATE_OPEN        0
#define SNDRV_PCM_STATE_SETUP       1
#define SNDRV_PCM_STATE_PREPARED    2
#define SNDRV_PCM_STATE_RUNNING     3
#define SNDRV_PCM_STATE_XRUN        4
#define SNDRV_PCM_STATE_DRAINING    5
#define SNDRV_PCM_STATE_PAUSED      6
#define SNDRV_PCM_STATE_SUSPENDED   7
#define SNDRV_PCM_STATE_DISCONNECTED 8

#define SNDRV_PCM_INFO_MMAP             0x00000001
#define SNDRV_PCM_INFO_MMAP_VALID       0x00000002
#define SNDRV_PCM_INFO_INTERLEAVED      0x00000100
#define SNDRV_PCM_INFO_BLOCK_TRANSFER   0x00010000
#define SNDRV_PCM_INFO_PAUSE            0x00080000

/* hw_params: three masks (bit sets), then twelve intervals [min, max] */
#define SNDRV_PCM_HW_PARAM_ACCESS       0
#define SNDRV_PCM_HW_PARAM_FORMAT       1
#define SNDRV_PCM_HW_PARAM_SUBFORMAT    2
#define SNDRV_PCM_HW_PARAM_FIRST_MASK   0
#define SNDRV_PCM_HW_PARAM_LAST_MASK    2
#define SNDRV_PCM_HW_PARAM_SAMPLE_BITS  8
#define SNDRV_PCM_HW_PARAM_FRAME_BITS   9
#define SNDRV_PCM_HW_PARAM_CHANNELS     10
#define SNDRV_PCM_HW_PARAM_RATE         11
#define SNDRV_PCM_HW_PARAM_PERIOD_TIME  12
#define SNDRV_PCM_HW_PARAM_PERIOD_SIZE  13
#define SNDRV_PCM_HW_PARAM_PERIOD_BYTES 14
#define SNDRV_PCM_HW_PARAM_PERIODS      15
#define SNDRV_PCM_HW_PARAM_BUFFER_TIME  16
#define SNDRV_PCM_HW_PARAM_BUFFER_SIZE  17
#define SNDRV_PCM_HW_PARAM_BUFFER_BYTES 18
#define SNDRV_PCM_HW_PARAM_TICK_TIME    19
#define SNDRV_PCM_HW_PARAM_FIRST_INTERVAL 8
#define SNDRV_PCM_HW_PARAM_LAST_INTERVAL  19
#define SNDRV_PCM_HW_PARAMS_NO_PERIOD_WAKEUP 4

#define SNDRV_MASK_MAX 256

struct snd_pcm_info {
    unsigned int device, subdevice;
    int stream, card;
    unsigned char id[64], name[80], subname[32];
    int dev_class, dev_subclass;
    unsigned int subdevices_count, subdevices_avail;
    unsigned char pad1[16], reserved[64];
};

struct snd_interval {
    unsigned int min, max;
    unsigned int openmin:1, openmax:1, integer:1, empty:1;
};

struct snd_mask {
    uint32_t bits[(SNDRV_MASK_MAX + 31) / 32];
};

struct snd_pcm_hw_params {
    unsigned int flags;
    struct snd_mask masks[3];
    struct snd_mask mres[5];
    struct snd_interval intervals[12];
    struct snd_interval ires[9];
    unsigned int rmask, cmask, info, msbits, rate_num, rate_den;
    snd_pcm_uframes_t fifo_size;
    unsigned char sync[16], reserved[48];
};

struct snd_pcm_sw_params {
    int tstamp_mode;
    unsigned int period_step, sleep_min;
    snd_pcm_uframes_t avail_min, xfer_align, start_threshold, stop_threshold,
                      silence_threshold, silence_size, boundary;
    unsigned int proto, tstamp_type;
    unsigned char reserved[56];
};

struct snd_pcm_channel_info {
    unsigned int channel;
    long offset;
    unsigned int first, step;
};

struct snd_pcm_status {
    int state;
    uint32_t pad1;
    struct snd_timespec trigger_tstamp, tstamp;
    snd_pcm_uframes_t appl_ptr, hw_ptr;
    snd_pcm_sframes_t delay;
    snd_pcm_uframes_t avail, avail_max, overrange;
    int suspended_state;
    uint32_t audio_tstamp_data;
    struct snd_timespec audio_tstamp, driver_tstamp;
    uint32_t audio_tstamp_accuracy;
    unsigned char reserved[52 - 2 * sizeof(struct snd_timespec)];
};

struct snd_pcm_mmap_status {
    int state;
    int pad1;
    snd_pcm_uframes_t hw_ptr;
    struct snd_timespec tstamp;
    int suspended_state;
    struct snd_timespec audio_tstamp;
};

struct snd_pcm_mmap_control {
    snd_pcm_uframes_t appl_ptr, avail_min;
};

#define SNDRV_PCM_SYNC_PTR_HWSYNC       1
#define SNDRV_PCM_SYNC_PTR_APPL         2
#define SNDRV_PCM_SYNC_PTR_AVAIL_MIN    4

struct snd_pcm_sync_ptr {
    unsigned int flags;
    union {
        struct snd_pcm_mmap_status status;
        unsigned char reserved[64];
    } s;
    union {
        struct snd_pcm_mmap_control control;
        unsigned char reserved[64];
    } c;
};

struct snd_xferi {
    snd_pcm_sframes_t result;
    void *buf;
    snd_pcm_uframes_t frames;
};

/* mmap offsets of the PCM device (we offer only the data: alsa-lib then
   reads status and control with SYNC_PTR) */
#define SNDRV_PCM_MMAP_OFFSET_DATA      0x00000000u
#define SNDRV_PCM_MMAP_OFFSET_STATUS    0x80000000u
#define SNDRV_PCM_MMAP_OFFSET_CONTROL   0x81000000u

#define SNDRV_PCM_IOCTL_PVERSION        SND_IOR('A', 0x00, int)
#define SNDRV_PCM_IOCTL_INFO            SND_IOR('A', 0x01, struct snd_pcm_info)
#define SNDRV_PCM_IOCTL_TSTAMP          SND_IOW('A', 0x02, int)
#define SNDRV_PCM_IOCTL_TTSTAMP         SND_IOW('A', 0x03, int)
#define SNDRV_PCM_IOCTL_USER_PVERSION   SND_IOW('A', 0x04, int)
#define SNDRV_PCM_IOCTL_HW_REFINE       SND_IOWR('A', 0x10, struct snd_pcm_hw_params)
#define SNDRV_PCM_IOCTL_HW_PARAMS       SND_IOWR('A', 0x11, struct snd_pcm_hw_params)
#define SNDRV_PCM_IOCTL_HW_FREE         SND_IO('A', 0x12)
#define SNDRV_PCM_IOCTL_SW_PARAMS       SND_IOWR('A', 0x13, struct snd_pcm_sw_params)
#define SNDRV_PCM_IOCTL_STATUS          SND_IOR('A', 0x20, struct snd_pcm_status)
#define SNDRV_PCM_IOCTL_DELAY           SND_IOR('A', 0x21, snd_pcm_sframes_t)
#define SNDRV_PCM_IOCTL_HWSYNC          SND_IO('A', 0x22)
#define SNDRV_PCM_IOCTL_SYNC_PTR        SND_IOWR('A', 0x23, struct snd_pcm_sync_ptr)
#define SNDRV_PCM_IOCTL_STATUS_EXT      SND_IOWR('A', 0x24, struct snd_pcm_status)
#define SNDRV_PCM_IOCTL_CHANNEL_INFO    SND_IOR('A', 0x32, struct snd_pcm_channel_info)
#define SNDRV_PCM_IOCTL_PREPARE         SND_IO('A', 0x40)
#define SNDRV_PCM_IOCTL_RESET           SND_IO('A', 0x41)
#define SNDRV_PCM_IOCTL_START           SND_IO('A', 0x42)
#define SNDRV_PCM_IOCTL_DROP            SND_IO('A', 0x43)
#define SNDRV_PCM_IOCTL_DRAIN           SND_IO('A', 0x44)
#define SNDRV_PCM_IOCTL_PAUSE           SND_IOW('A', 0x45, int)
#define SNDRV_PCM_IOCTL_REWIND          SND_IOW('A', 0x46, snd_pcm_uframes_t)
#define SNDRV_PCM_IOCTL_RESUME          SND_IO('A', 0x47)
#define SNDRV_PCM_IOCTL_XRUN            SND_IO('A', 0x48)
#define SNDRV_PCM_IOCTL_FORWARD         SND_IOW('A', 0x49, snd_pcm_uframes_t)
#define SNDRV_PCM_IOCTL_WRITEI_FRAMES   SND_IOW('A', 0x50, struct snd_xferi)
#define SNDRV_PCM_IOCTL_READI_FRAMES    SND_IOR('A', 0x51, struct snd_xferi)
#define SNDRV_PCM_IOCTL_LINK            SND_IOW('A', 0x60, int)
#define SNDRV_PCM_IOCTL_UNLINK          SND_IO('A', 0x61)

/* --- controls (the mixer) ----------------------------------------------------- */

#define SNDRV_CTL_ELEM_ID_NAME_MAXLEN   44

#define SNDRV_CTL_ELEM_TYPE_BOOLEAN     1
#define SNDRV_CTL_ELEM_TYPE_INTEGER     2

#define SNDRV_CTL_ELEM_IFACE_CARD       0
#define SNDRV_CTL_ELEM_IFACE_MIXER      2

#define SNDRV_CTL_ELEM_ACCESS_READ      (1 << 0)
#define SNDRV_CTL_ELEM_ACCESS_WRITE     (1 << 1)
#define SNDRV_CTL_ELEM_ACCESS_VOLATILE  (1 << 2)
#define SNDRV_CTL_ELEM_ACCESS_TLV_READ  (1 << 4)

#define SNDRV_CTL_EVENT_ELEM            0
#define SNDRV_CTL_EVENT_MASK_VALUE      (1 << 0)

#define SNDRV_CTL_TLVT_DB_SCALE         1
#define SNDRV_CTL_TLVD_DB_SCALE_MUTE    0x10000

struct snd_ctl_card_info {
    int card, pad;
    unsigned char id[16], driver[16], name[32], longname[80], reserved_[16],
                  mixername[80], components[128];
};

struct snd_ctl_elem_id {
    unsigned int numid;
    int iface;
    unsigned int device, subdevice;
    unsigned char name[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];
    unsigned int index;
};

struct snd_ctl_elem_list {
    unsigned int offset, space, used, count;
    struct snd_ctl_elem_id *pids;
    unsigned char reserved[50];
};

struct snd_ctl_elem_info {
    struct snd_ctl_elem_id id;
    int type;
    unsigned int access, count;
    int owner;
    union {
        struct { long min, max, step; } integer;
        struct { long long min, max, step; } integer64;
        struct {
            unsigned int items, item;
            char name[64];
            uint64_t names_ptr;
            unsigned int names_length;
        } enumerated;
        unsigned char reserved[128];
    } value;
    unsigned char reserved[64];
};

struct snd_ctl_elem_value {
    struct snd_ctl_elem_id id;
    unsigned int indirect:1;
    union {
        long integer[128];
        long long integer64[64];
        unsigned int enumerated[128];
        unsigned char bytes[512];
    } value;
    unsigned char reserved[128];
};

struct snd_ctl_tlv {
    unsigned int numid, length;
    unsigned int tlv[];
};

struct snd_ctl_event {
    int type;
    union {
        struct {
            unsigned int mask;
            struct snd_ctl_elem_id id;
        } elem;
        unsigned char data8[60];
    } data;
};

#define SNDRV_CTL_IOCTL_PVERSION        SND_IOR('U', 0x00, int)
#define SNDRV_CTL_IOCTL_CARD_INFO       SND_IOR('U', 0x01, struct snd_ctl_card_info)
#define SNDRV_CTL_IOCTL_ELEM_LIST       SND_IOWR('U', 0x10, struct snd_ctl_elem_list)
#define SNDRV_CTL_IOCTL_ELEM_INFO       SND_IOWR('U', 0x11, struct snd_ctl_elem_info)
#define SNDRV_CTL_IOCTL_ELEM_READ       SND_IOWR('U', 0x12, struct snd_ctl_elem_value)
#define SNDRV_CTL_IOCTL_ELEM_WRITE      SND_IOWR('U', 0x13, struct snd_ctl_elem_value)
#define SNDRV_CTL_IOCTL_ELEM_LOCK       SND_IOW('U', 0x14, struct snd_ctl_elem_id)
#define SNDRV_CTL_IOCTL_ELEM_UNLOCK     SND_IOW('U', 0x15, struct snd_ctl_elem_id)
#define SNDRV_CTL_IOCTL_SUBSCRIBE_EVENTS SND_IOWR('U', 0x16, int)
#define SNDRV_CTL_IOCTL_TLV_READ        SND_IOWR('U', 0x1a, struct snd_ctl_tlv)
#define SNDRV_CTL_IOCTL_HWDEP_NEXT_DEVICE SND_IOWR('U', 0x20, int)
#define SNDRV_CTL_IOCTL_PCM_NEXT_DEVICE SND_IOR('U', 0x30, int)
#define SNDRV_CTL_IOCTL_PCM_INFO        SND_IOWR('U', 0x31, struct snd_pcm_info)
#define SNDRV_CTL_IOCTL_PCM_PREFER_SUBDEVICE SND_IOW('U', 0x32, int)
#define SNDRV_CTL_IOCTL_RAWMIDI_NEXT_DEVICE SND_IOWR('U', 0x40, int)
#define SNDRV_CTL_IOCTL_UMP_NEXT_DEVICE SND_IOWR('U', 0x43, int)
#define SNDRV_CTL_IOCTL_POWER           SND_IOWR('U', 0xd0, int)
#define SNDRV_CTL_IOCTL_POWER_STATE     SND_IOR('U', 0xd1, int)

#endif
