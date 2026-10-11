/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * hda_verbs.h — the commands ("verbs") an HD Audio codec understands, and
 * the parameters it reports. From Intel's High Definition Audio
 * Specification 1.0a, chapter 7; the names are Linux's
 * (include/sound/hda_verbs.h), so a driver reads the same on both.
 *
 * A verb travels as one 32-bit word: codec address (4 bits), node id (8),
 * then either a 12-bit verb with an 8-bit payload or, for a few verbs, a
 * 4-bit verb with a 16-bit payload.
 */
#ifndef SOUND_HDA_VERBS_H
#define SOUND_HDA_VERBS_H

#define AC_NODE_ROOT            0x00

/* Verbs that read */
#define AC_VERB_PARAMETERS              0xf00
#define AC_VERB_GET_CONNECT_SEL         0xf01
#define AC_VERB_GET_CONNECT_LIST        0xf02
#define AC_VERB_GET_POWER_STATE         0xf05
#define AC_VERB_GET_PIN_WIDGET_CONTROL  0xf07
#define AC_VERB_GET_PIN_SENSE           0xf09
#define AC_VERB_GET_EAPD_BTLENABLE      0xf0c
#define AC_VERB_GET_CONFIG_DEFAULT      0xf1c
#define AC_VERB_GET_SUBSYSTEM_ID        0xf20

/* Verbs that set */
#define AC_VERB_SET_POWER_STATE         0x705
#define AC_VERB_SET_PIN_WIDGET_CONTROL  0x707
#define AC_VERB_SET_UNSOLICITED_ENABLE  0x708
#define AC_VERB_SET_EAPD_BTLENABLE      0x70c
#define AC_VERB_SET_CODEC_RESET         0x7ff

/* Parameters (AC_VERB_PARAMETERS) */
#define AC_PAR_VENDOR_ID                0x00
#define AC_PAR_SUBSYSTEM_ID             0x01
#define AC_PAR_REV_ID                   0x02
#define AC_PAR_NODE_COUNT               0x04    /* first node << 16 | count */
#define AC_PAR_FUNCTION_TYPE            0x05
#define AC_PAR_AUDIO_FG_CAP             0x08
#define AC_PAR_AUDIO_WIDGET_CAP         0x09
#define AC_PAR_PCM                      0x0a
#define AC_PAR_STREAM                   0x0b
#define AC_PAR_PIN_CAP                  0x0c
#define AC_PAR_AMP_IN_CAP               0x0d
#define AC_PAR_CONNLIST_LEN             0x0e
#define AC_PAR_POWER_STATE              0x0f
#define AC_PAR_AMP_OUT_CAP              0x12

/* AC_PAR_FUNCTION_TYPE */
#define AC_FGT_TYPE                     0xff
#define AC_GRP_AUDIO_FUNCTION           0x01
#define AC_GRP_MODEM_FUNCTION           0x02

/* AC_PAR_AUDIO_WIDGET_CAP: bits 23:20 are the widget's type */
#define AC_WCAP_TYPE_SHIFT              20
#define AC_WID_AUD_OUT                  0x0     /* a DAC */
#define AC_WID_AUD_IN                   0x1     /* an ADC */
#define AC_WID_AUD_MIX                  0x2
#define AC_WID_AUD_SEL                  0x3
#define AC_WID_PIN                      0x4
#define AC_WID_POWER                    0x5
#define AC_WID_VOL_KNB                  0x6
#define AC_WID_BEEP                     0x7
#define AC_WID_VENDOR                   0xf

/* A pin's default configuration (AC_VERB_GET_CONFIG_DEFAULT), written by
   the BIOS: what the pin is wired to on this machine. */
#define AC_DEFCFG_PORT_CONN_SHIFT       30      /* jack, none, internal, both */
#define AC_DEFCFG_LOCATION_SHIFT        24
#define AC_DEFCFG_DEVICE_SHIFT          20
#define AC_DEFCFG_CONN_TYPE_SHIFT       16
#define AC_DEFCFG_COLOR_SHIFT           12
#define AC_DEFCFG_ASSOC_SHIFT           4

#define AC_JACK_PORT_COMPLEX            0
#define AC_JACK_PORT_NONE               1
#define AC_JACK_PORT_FIXED              2
#define AC_JACK_PORT_BOTH               3

#define AC_JACK_LINE_OUT                0x0
#define AC_JACK_SPEAKER                 0x1
#define AC_JACK_HP_OUT                  0x2
#define AC_JACK_CD                      0x3
#define AC_JACK_SPDIF_OUT               0x4
#define AC_JACK_DIG_OTHER_OUT           0x5
#define AC_JACK_LINE_IN                 0x8
#define AC_JACK_AUX                     0x9
#define AC_JACK_MIC_IN                  0xa
#define AC_JACK_SPDIF_IN                0xc
#define AC_JACK_DIG_OTHER_IN            0xd
#define AC_JACK_OTHER                   0xf

/* 4-bit verbs with a 16-bit payload (snd_hdac_make_cmd: verb << 8) */
#define AC_VERB_SET_STREAM_FORMAT       0x200
#define AC_VERB_SET_AMP_GAIN_MUTE       0x300
#define AC_VERB_GET_AMP_GAIN_MUTE       0xb00
#define AC_VERB_SET_CONNECT_SEL         0x701
#define AC_VERB_SET_CHANNEL_STREAMID    0x706
#define AC_VERB_SET_PIN_SENSE           0x709

/* AC_PAR_AUDIO_WIDGET_CAP bits */
#define AC_WCAP_STEREO          (1 << 0)
#define AC_WCAP_IN_AMP          (1 << 1)
#define AC_WCAP_OUT_AMP         (1 << 2)
#define AC_WCAP_AMP_OVRD        (1 << 3)
#define AC_WCAP_FORMAT_OVRD     (1 << 4)
#define AC_WCAP_UNSOL_CAP       (1 << 7)
#define AC_WCAP_CONN_LIST       (1 << 8)
#define AC_WCAP_DIGITAL         (1 << 9)
#define AC_WCAP_POWER           (1 << 10)

/* AC_PAR_PIN_CAP bits */
#define AC_PINCAP_TRIG_REQ      (1 << 1)
#define AC_PINCAP_PRES_DETECT   (1 << 2)
#define AC_PINCAP_HP_DRV        (1 << 3)
#define AC_PINCAP_OUT           (1 << 4)
#define AC_PINCAP_IN            (1 << 5)
#define AC_PINCAP_VREF_SHIFT    8           /* bits 15:8: which VREFs */
#define AC_PINCAP_EAPD          (1 << 16)

/* AC_VERB_SET_PIN_WIDGET_CONTROL */
#define AC_PINCTL_VREF_HIZ      0
#define AC_PINCTL_VREF_50       1
#define AC_PINCTL_VREF_80       4
#define AC_PINCTL_VREF_100      5
#define AC_PINCTL_IN_EN         (1 << 5)
#define AC_PINCTL_OUT_EN        (1 << 6)
#define AC_PINCTL_HP_EN         (1 << 7)

/* AC_VERB_GET_PIN_SENSE */
#define AC_PINSENSE_PRESENCE    (1u << 31)

/* AC_VERB_SET_EAPD_BTLENABLE */
#define AC_EAPDBTL_EAPD         (1 << 1)

/* AC_VERB_SET_UNSOLICITED_ENABLE: enable, and the tag that comes back in
   the response's bits 31:26 */
#define AC_USRSP_EN             (1 << 7)
#define AC_UNSOL_RES_TAG_SHIFT  26

/* Amplifier capabilities (AC_PAR_AMP_IN_CAP / AC_PAR_AMP_OUT_CAP) */
#define AC_AMPCAP_OFFSET        0x7f        /* the step that is 0 dB */
#define AC_AMPCAP_NUM_STEPS_SHIFT 8         /* bits 14:8: the highest step */
#define AC_AMPCAP_STEP_SIZE_SHIFT 16        /* bits 22:16: (0.25 dB units) - 1 */
#define AC_AMPCAP_MUTE          (1u << 31)

/* AC_VERB_SET_AMP_GAIN_MUTE payload */
#define AC_AMP_SET_OUTPUT       (1 << 15)
#define AC_AMP_SET_INPUT        (1 << 14)
#define AC_AMP_SET_LEFT         (1 << 13)
#define AC_AMP_SET_RIGHT        (1 << 12)
#define AC_AMP_SET_INDEX_SHIFT  8
#define AC_AMP_MUTE             (1 << 7)

/* AC_PAR_PCM: rates in bits 11:0, sample sizes in bits 20:16 */
#define AC_SUPPCM_BITS_16       (1 << 17)
#define AC_SUPPCM_BITS_24       (1 << 19)
#define AC_SUPPCM_BITS_32       (1 << 20)

/* Power states (AC_VERB_SET_POWER_STATE) */
#define AC_PWRST_D0                     0x00
#define AC_PWRST_D3                     0x03

#endif
