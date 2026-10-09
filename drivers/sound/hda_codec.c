/*
 * hda_codec.c — the generic HD Audio codec driver: speakers, headphones,
 * line out and microphones on any codec that follows the HD Audio spec.
 * Ours, from Intel's HD Audio Specification 1.0a, chapter 7; Linux's
 * names for what programs see ("Master Playback Volume", "Headphone
 * Jack"), so alsamixer and PulseAudio find what they look for. Linux, too,
 * runs the MSI's Realtek codec with its generic driver (docs/SOUND-PLAN.md).
 *
 * A codec is a graph of widgets. Sound to the speaker flows
 *
 *     DAC (digital -> analog) -> [mixer / selector ...] -> pin (the speaker)
 *
 * and each widget lists the widgets it takes sound from: its connection
 * list. So a path is found backwards, from the pin to a DAC; for recording
 * forwards, from the ADC (analog -> digital) to a microphone pin. Turning a
 * path on means: power every widget on it, pick the right input where a
 * widget has a choice (a selector) or unmute only that input (a mixer, which
 * adds all of its inputs up), unmute the amplifiers along it, enable the
 * pin's output or input, and the external speaker amplifier (EAPD).
 *
 * The BIOS wrote into each pin what it is wired to (its default
 * configuration): speaker, headphone jack, internal mic... That, not the
 * pin's number, tells us which pin is the speaker.
 *
 * All playback outputs listen to the one playback stream: each DAC on a
 * path is set to the stream's tag, so speaker and headphones play the same
 * sound; plugging headphones in turns the speaker pins off (the codec
 * tells us with an unsolicited response; hda_intel.c's jack thread calls
 * hda_gen_unsol). Codec commands sleep, so all of this runs in threads,
 * never in the interrupt handler.
 */
#include <errno.h>
#include <kheap.h>
#include <kprintf.h>
#include <sound/core.h>
#include <sound/hdaudio.h>
#include <string.h>

#define MAX_WIDGETS 96
#define MAX_CONN    16
#define MAX_DEPTH   6
#define MAX_OUTS    4
#define MAX_INS     3

enum { OUT_LINE, OUT_SPEAKER, OUT_HP };

struct widget {
    uint8_t type, nconn;
    uint32_t wcaps, pincap, defcfg, ampin, ampout;
    hda_nid_t conn[MAX_CONN];
};

/* nid[0] is where the search began (a pin for output, the ADC for input),
   nid[n - 1] where it ended; idx[k] is the input of nid[k] that leads to
   nid[k + 1]. */
struct path {
    hda_nid_t nid[MAX_DEPTH];
    uint8_t idx[MAX_DEPTH];
    int n;
};

/* An amplifier: a widget's output amp, or one of its input amps. */
struct amp {
    hda_nid_t nid;
    uint8_t out, idx;
    uint32_t caps;
};

struct output {
    hda_nid_t pin;
    int kind, jack;                         /* jack: it can tell plugged or not */
    struct path path;                       /* pin ... DAC */
    struct amp vol, mute;                   /* nid 0: none */
};

struct input {
    hda_nid_t pin;
    int internal, jack;
    struct path path;                       /* ADC ... pin */
    struct amp boost;
};

struct hda_gen {
    struct hdac_device *codec;
    struct widget w[MAX_WIDGETS];
    struct output outs[MAX_OUTS];
    struct input ins[MAX_INS];
    int nouts, nins, cur_in;
    hda_nid_t dacs[MAX_OUTS];
    int ndacs;
    hda_nid_t adc;
    struct amp cap_vol;
    uint32_t pcm_out, pcm_in;               /* AC_PAR_PCM of the converters */
    long master[2], capture[2], boost[2];
    int master_on[2], capture_on[2];
    int hp_in, mic_in, have_hp_jack, have_mic_jack;
    struct snd_card *card;
    struct snd_kcontrol *hp_jack, *mic_jack;
    unsigned int dbtlv[3][4];               /* the dB scales of Master, Capture, Boost */
    char chip[24];
};

static struct widget *wid(struct hda_gen *g, hda_nid_t nid)
{
    int k = nid - g->codec->start_nid;
    return k >= 0 && k < g->codec->num_nodes && k < MAX_WIDGETS ? &g->w[k] : NULL;
}

static unsigned int rd(struct hda_gen *g, hda_nid_t nid, unsigned int verb, unsigned int parm)
{
    return (unsigned int)snd_hdac_codec_read(g->codec, nid, 0, verb, parm);
}

static void wr(struct hda_gen *g, hda_nid_t nid, unsigned int verb, unsigned int parm)
{
    snd_hdac_codec_write(g->codec, nid, 0, verb, parm);
}

/* A widget's connection list: 4 short entries (8 bits) or 2 long (16 bits)
   per answer; an entry with the range bit means "everything from the
   previous one up to this one". */
static void read_conns(struct hda_gen *g, hda_nid_t nid, struct widget *w)
{
    int len = snd_hdac_read_parm(g->codec, nid, AC_PAR_CONNLIST_LEN);
    if (len < 0 || !(w->wcaps & AC_WCAP_CONN_LIST))
        return;
    int longform = len & 0x80, n = len & 0x7f, per = longform ? 2 : 4;
    unsigned int bits = longform ? 16 : 8, mask = (1u << (bits - 1)) - 1, range = 1u << (bits - 1);
    hda_nid_t prev = 0;
    for (int i = 0; i < n; i++) {
        unsigned int word = rd(g, nid, AC_VERB_GET_CONNECT_LIST, (unsigned int)(i - i % per));
        unsigned int e = (word >> (bits * (unsigned int)(i % per))) & ((1u << bits) - 1);
        hda_nid_t v = (hda_nid_t)(e & mask);
        if ((e & range) && prev && v > prev) {
            for (hda_nid_t x = (hda_nid_t)(prev + 1); x <= v && w->nconn < MAX_CONN; x++)
                w->conn[w->nconn++] = x;
        } else if (w->nconn < MAX_CONN)
            w->conn[w->nconn++] = v;
        prev = v;
    }
}

/* Depth-first, backwards along connection lists, from nid to a widget of
   type `want` (an unused DAC if there is one: avoid != NULL). */
static int find_path(struct hda_gen *g, hda_nid_t nid, int want, struct path *p, int depth,
                     const hda_nid_t *avoid, int navoid)
{
    struct widget *w = wid(g, nid);
    if (!w || depth >= MAX_DEPTH || (w->wcaps & AC_WCAP_DIGITAL))
        return 0;
    for (int k = 0; k < depth; k++)
        if (p->nid[k] == nid)
            return 0;                       /* a loop */
    p->nid[depth] = nid;
    p->n = depth + 1;
    if (w->type == want) {
        for (int k = 0; k < navoid; k++)
            if (avoid[k] == nid)
                return 0;
        return 1;
    }
    if (depth > 0 && w->type == AC_WID_PIN)
        return 0;                           /* not through another pin */
    for (int i = 0; i < w->nconn; i++) {
        p->idx[depth] = (uint8_t)i;
        if (find_path(g, w->conn[i], want, p, depth + 1, avoid, navoid))
            return 1;
    }
    return 0;
}

/* Recording paths start at the ADC and end at the pin. */
static int find_in_path(struct hda_gen *g, hda_nid_t nid, hda_nid_t pin, struct path *p, int depth)
{
    struct widget *w = wid(g, nid);
    if (!w || depth >= MAX_DEPTH)
        return 0;
    for (int k = 0; k < depth; k++)
        if (p->nid[k] == nid)
            return 0;
    p->nid[depth] = nid;
    p->n = depth + 1;
    if (nid == pin)
        return 1;
    if (depth > 0 && w->type == AC_WID_PIN)
        return 0;
    for (int i = 0; i < w->nconn; i++) {
        p->idx[depth] = (uint8_t)i;
        if (find_in_path(g, w->conn[i], pin, p, depth + 1))
            return 1;
    }
    return 0;
}

/* --- amplifiers ------------------------------------------------------------------- */

static unsigned int amp_steps(uint32_t caps)
{
    return (caps >> AC_AMPCAP_NUM_STEPS_SHIFT) & 0x7f;
}

static void amp_set(struct hda_gen *g, const struct amp *a, int ch, unsigned int gain, int mute)
{
    unsigned int v = (a->out ? AC_AMP_SET_OUTPUT : AC_AMP_SET_INPUT) |
                     (ch == 0 ? AC_AMP_SET_LEFT : ch == 1 ? AC_AMP_SET_RIGHT
                                                          : AC_AMP_SET_LEFT | AC_AMP_SET_RIGHT) |
                     (unsigned int)a->idx << AC_AMP_SET_INDEX_SHIFT |
                     (mute ? AC_AMP_MUTE : 0) | (gain & 0x7f);
    wr(g, a->nid, AC_VERB_SET_AMP_GAIN_MUTE, v);
}

static struct amp amp_of(struct hda_gen *g, hda_nid_t nid, int out, int idx)
{
    struct widget *w = wid(g, nid);
    struct amp a = { .nid = nid, .out = (uint8_t)out, .idx = (uint8_t)idx };
    a.caps = out ? w->ampout : w->ampin;
    return a;
}

/* An amp's dB scale for the control's TLV: 0.25 dB steps * (size + 1). */
static void db_tlv(unsigned int tlv[4], uint32_t caps)
{
    int step = (int)(((caps >> AC_AMPCAP_STEP_SIZE_SHIFT) & 0x7f) + 1) * 25;   /* 0.01 dB */
    int offset = (int)(caps & AC_AMPCAP_OFFSET);
    tlv[0] = SNDRV_CTL_TLVT_DB_SCALE;
    tlv[1] = 2 * sizeof(unsigned int);
    tlv[2] = (unsigned int)(-offset * step);
    tlv[3] = (unsigned int)step | (caps & AC_AMPCAP_MUTE ? SNDRV_CTL_TLVD_DB_SCALE_MUTE : 0);
}

/* Turn a path on: power, choices, amps at 0 dB, unmuted. Mixers: only the
   input on the path is heard (the others — say a microphone looped back
   to the speakers — are muted). */
static void path_on(struct hda_gen *g, const struct path *p)
{
    for (int k = 0; k < p->n; k++) {
        hda_nid_t nid = p->nid[k];
        struct widget *w = wid(g, nid);
        if (w->wcaps & AC_WCAP_POWER)
            wr(g, nid, AC_VERB_SET_POWER_STATE, AC_PWRST_D0);
        if (w->wcaps & AC_WCAP_OUT_AMP)
            amp_set(g, &(struct amp){ nid, 1, 0, w->ampout }, -1, w->ampout & AC_AMPCAP_OFFSET, 0);
        if (k == p->n - 1)
            continue;                       /* the far end: no input chosen */
        if (w->type == AC_WID_AUD_MIX) {
            if (w->wcaps & AC_WCAP_IN_AMP)
                for (int i = 0; i < w->nconn; i++)
                    amp_set(g, &(struct amp){ nid, 0, (uint8_t)i, w->ampin }, -1,
                            w->ampin & AC_AMPCAP_OFFSET, i != p->idx[k]);
        } else {
            if (w->nconn > 1)
                wr(g, nid, AC_VERB_SET_CONNECT_SEL, p->idx[k]);
            if (w->wcaps & AC_WCAP_IN_AMP)
                amp_set(g, &(struct amp){ nid, 0, p->idx[k], w->ampin }, -1,
                        w->ampin & AC_AMPCAP_OFFSET, 0);
        }
    }
}

/* --- jacks: speakers off while headphones are in ---------------------------------- */

static int sense(struct hda_gen *g, hda_nid_t pin)
{
    struct widget *w = wid(g, pin);
    if (w->pincap & AC_PINCAP_TRIG_REQ)
        wr(g, pin, AC_VERB_SET_PIN_SENSE, 0);
    return (rd(g, pin, AC_VERB_GET_PIN_SENSE, 0) & AC_PINSENSE_PRESENCE) != 0;
}

static void apply_outputs(struct hda_gen *g)
{
    for (int i = 0; i < g->nouts; i++) {
        struct output *o = &g->outs[i];
        struct widget *w = wid(g, o->pin);
        int on = o->kind == OUT_HP || !g->hp_in;
        unsigned int ctl = on ? AC_PINCTL_OUT_EN : 0;
        if (on && o->kind == OUT_HP && (w->pincap & AC_PINCAP_HP_DRV))
            ctl |= AC_PINCTL_HP_EN;
        wr(g, o->pin, AC_VERB_SET_PIN_WIDGET_CONTROL, ctl);
    }
}

/* Recording from the jack's mic when one is in, else the built-in one. */
static void apply_input(struct hda_gen *g)
{
    int pick = -1;
    for (int i = 0; i < g->nins; i++)
        if (pick < 0 || (g->mic_in ? !g->ins[i].internal : g->ins[i].internal))
            pick = i;
    if (pick < 0)
        return;
    g->cur_in = pick;
    struct input *in = &g->ins[pick];
    struct widget *w = wid(g, in->pin);
    unsigned int vrefs = (w->pincap >> AC_PINCAP_VREF_SHIFT) & 0xff, ctl = AC_PINCTL_IN_EN;
    ctl |= vrefs & (1 << AC_PINCTL_VREF_80) ? AC_PINCTL_VREF_80     /* mic bias: 80%, */
         : vrefs & (1 << AC_PINCTL_VREF_50) ? AC_PINCTL_VREF_50 : 0; /* else 50% */
    wr(g, in->pin, AC_VERB_SET_PIN_WIDGET_CONTROL, ctl);
    path_on(g, &in->path);
}

static void check_jacks(struct hda_gen *g, int notify)
{
    int hp = 0, mic = 0;
    for (int i = 0; i < g->nouts; i++)
        if (g->outs[i].jack && g->outs[i].kind == OUT_HP)
            hp |= sense(g, g->outs[i].pin);
    for (int i = 0; i < g->nins; i++)
        if (g->ins[i].jack)
            mic |= sense(g, g->ins[i].pin);
    int hp_changed = hp != g->hp_in, mic_changed = mic != g->mic_in;
    g->hp_in = hp;
    g->mic_in = mic;
    if (hp_changed || !notify)
        apply_outputs(g);
    if (mic_changed || !notify)
        apply_input(g);
    if (notify && hp_changed && g->hp_jack)
        snd_ctl_notify(g->card, SNDRV_CTL_EVENT_MASK_VALUE, &g->hp_jack->id);
    if (notify && mic_changed && g->mic_jack)
        snd_ctl_notify(g->card, SNDRV_CTL_EVENT_MASK_VALUE, &g->mic_jack->id);
}

void hda_gen_unsol(struct hda_gen *g, unsigned int res)
{
    (void)res;                          /* any jack: look at them all */
    check_jacks(g, 1);
}

/* --- finding the outputs and inputs -------------------------------------------------- */

static int dac_used(struct hda_gen *g, hda_nid_t dac)
{
    for (int i = 0; i < g->ndacs; i++)
        if (g->dacs[i] == dac)
            return 1;
    return 0;
}

static void add_output(struct hda_gen *g, hda_nid_t pin, int kind)
{
    if (g->nouts == MAX_OUTS)
        return;
    struct output *o = &g->outs[g->nouts];
    memset(o, 0, sizeof *o);
    /* a DAC of its own if one is free, else share */
    if (!find_path(g, pin, AC_WID_AUD_OUT, &o->path, 0, g->dacs, g->ndacs) &&
        !find_path(g, pin, AC_WID_AUD_OUT, &o->path, 0, NULL, 0))
        return;
    o->pin = pin;
    o->kind = kind;
    struct widget *pw = wid(g, pin);
    o->jack = (pw->pincap & AC_PINCAP_PRES_DETECT) &&
              ((pw->defcfg >> AC_DEFCFG_PORT_CONN_SHIFT) & 3) != AC_JACK_PORT_FIXED;
    hda_nid_t dac = o->path.nid[o->path.n - 1];
    if (!dac_used(g, dac) && g->ndacs < MAX_OUTS)
        g->dacs[g->ndacs++] = dac;
    /* The volume: the amp with steps nearest the DAC; the mute: the first
       amp that can mute, from the same end. */
    for (int k = o->path.n - 1; k >= 0; k--) {
        struct widget *w = wid(g, o->path.nid[k]);
        struct amp a = { 0 };
        if ((w->wcaps & AC_WCAP_OUT_AMP))
            a = amp_of(g, o->path.nid[k], 1, 0);
        else if ((w->wcaps & AC_WCAP_IN_AMP) && k < o->path.n - 1)
            a = amp_of(g, o->path.nid[k], 0, o->path.idx[k]);
        if (!a.nid)
            continue;
        if (!o->vol.nid && amp_steps(a.caps))
            o->vol = a;
        if (!o->mute.nid && (a.caps & AC_AMPCAP_MUTE))
            o->mute = a;
    }
    g->nouts++;
}

static void add_input(struct hda_gen *g, hda_nid_t pin, int internal)
{
    if (g->nins == MAX_INS)
        return;
    struct input *in = &g->ins[g->nins];
    memset(in, 0, sizeof *in);
    for (int n = 0; n < g->codec->num_nodes && n < MAX_WIDGETS; n++) {
        hda_nid_t adc = (hda_nid_t)(g->codec->start_nid + n);
        struct widget *w = &g->w[n];
        if (w->type != AC_WID_AUD_IN || (w->wcaps & AC_WCAP_DIGITAL))
            continue;
        if (g->adc && adc != g->adc)
            continue;                       /* one ADC records, for every mic */
        if (find_in_path(g, adc, pin, &in->path, 0)) {
            g->adc = adc;
            break;
        }
    }
    if (!in->path.n || in->path.nid[in->path.n - 1] != pin)
        return;
    in->pin = pin;
    in->internal = internal;
    struct widget *pw = wid(g, pin);
    in->jack = !internal && (pw->pincap & AC_PINCAP_PRES_DETECT);
    if ((pw->wcaps & AC_WCAP_IN_AMP) && amp_steps(pw->ampin))
        in->boost = amp_of(g, pin, 0, 0);
    g->nins++;
}

/* "ALC256" for Realtek's 10ec:0256, as Linux names it; others by number. */
static void codec_name(struct hda_gen *g)
{
    static const char hex[] = "0123456789abcdef";
    uint32_t id = g->codec->vendor_id;
    int realtek = (id >> 16) == 0x10ec, digits = realtek ? 3 : 4;
    const char *pre = realtek ? "ALC" : (id >> 16) == 0x1af4 ? "QEMU " : "HDA ";
    char *p = g->chip;
    while (*pre)
        *p++ = *pre++;
    for (int k = digits - 1; k >= 0; k--)
        *p++ = hex[(id >> (4 * k)) & 0xf];
    *p = 0;
}

struct hda_gen *hda_gen_probe(struct hdac_device *codec)
{
    if (!codec->afg || codec->num_nodes <= 0)
        return NULL;
    struct hda_gen *g = kmalloc(sizeof *g);
    if (!g)
        return NULL;
    memset(g, 0, sizeof *g);
    g->codec = codec;
    hda_nid_t afg = codec->afg;
    wr(g, afg, AC_VERB_SET_POWER_STATE, AC_PWRST_D0);
    uint32_t def_in = (uint32_t)snd_hdac_read_parm(codec, afg, AC_PAR_AMP_IN_CAP);
    uint32_t def_out = (uint32_t)snd_hdac_read_parm(codec, afg, AC_PAR_AMP_OUT_CAP);
    uint32_t def_pcm = (uint32_t)snd_hdac_read_parm(codec, afg, AC_PAR_PCM);

    for (int n = 0; n < codec->num_nodes && n < MAX_WIDGETS; n++) {
        hda_nid_t nid = (hda_nid_t)(codec->start_nid + n);
        struct widget *w = &g->w[n];
        w->wcaps = (uint32_t)snd_hdac_read_parm(codec, nid, AC_PAR_AUDIO_WIDGET_CAP);
        w->type = (uint8_t)((w->wcaps >> AC_WCAP_TYPE_SHIFT) & 0xf);
        w->ampin = w->wcaps & AC_WCAP_AMP_OVRD
                 ? (uint32_t)snd_hdac_read_parm(codec, nid, AC_PAR_AMP_IN_CAP) : def_in;
        w->ampout = w->wcaps & AC_WCAP_AMP_OVRD
                  ? (uint32_t)snd_hdac_read_parm(codec, nid, AC_PAR_AMP_OUT_CAP) : def_out;
        if (w->type == AC_WID_PIN) {
            w->pincap = (uint32_t)snd_hdac_read_parm(codec, nid, AC_PAR_PIN_CAP);
            w->defcfg = rd(g, nid, AC_VERB_GET_CONFIG_DEFAULT, 0);
        }
        read_conns(g, nid, w);
    }

    /* Outputs: speakers first (they are the ones always there), then
       headphones, then line outs. */
    static const int order[3][2] = { { AC_JACK_SPEAKER, OUT_SPEAKER },
                                     { AC_JACK_HP_OUT, OUT_HP }, { AC_JACK_LINE_OUT, OUT_LINE } };
    for (int pass = 0; pass < 3; pass++)
        for (int n = 0; n < codec->num_nodes && n < MAX_WIDGETS; n++) {
            struct widget *w = &g->w[n];
            unsigned int conn = (w->defcfg >> AC_DEFCFG_PORT_CONN_SHIFT) & 3;
            unsigned int dev = (w->defcfg >> AC_DEFCFG_DEVICE_SHIFT) & 0xf;
            if (w->type != AC_WID_PIN || conn == AC_JACK_PORT_NONE ||
                (w->wcaps & AC_WCAP_DIGITAL) || !(w->pincap & AC_PINCAP_OUT))
                continue;
            if ((int)dev == order[pass][0])
                add_output(g, (hda_nid_t)(codec->start_nid + n), order[pass][1]);
        }
    if (!g->nouts) {
        kfree(g);
        return NULL;                        /* no analog outputs: HDMI, a modem */
    }
    for (int n = 0; n < codec->num_nodes && n < MAX_WIDGETS; n++) {
        struct widget *w = &g->w[n];
        unsigned int conn = (w->defcfg >> AC_DEFCFG_PORT_CONN_SHIFT) & 3;
        unsigned int dev = (w->defcfg >> AC_DEFCFG_DEVICE_SHIFT) & 0xf;
        if (w->type == AC_WID_PIN && conn != AC_JACK_PORT_NONE && !(w->wcaps & AC_WCAP_DIGITAL) &&
            (w->pincap & AC_PINCAP_IN) && (dev == AC_JACK_MIC_IN || dev == AC_JACK_LINE_IN))
            add_input(g, (hda_nid_t)(codec->start_nid + n),
                      conn == AC_JACK_PORT_FIXED || ((w->defcfg >> AC_DEFCFG_LOCATION_SHIFT) & 0x30) == 0x10);
    }
    if (g->adc) {
        struct widget *aw = wid(g, g->adc);
        if ((aw->wcaps & AC_WCAP_IN_AMP) && amp_steps(aw->ampin))
            g->cap_vol = amp_of(g, g->adc, 0, 0);   /* index set per input */
        g->pcm_in = aw->wcaps & AC_WCAP_FORMAT_OVRD
                  ? (uint32_t)snd_hdac_read_parm(codec, g->adc, AC_PAR_PCM) : def_pcm;
    }
    struct widget *dw = wid(g, g->dacs[0]);
    g->pcm_out = dw->wcaps & AC_WCAP_FORMAT_OVRD
               ? (uint32_t)snd_hdac_read_parm(codec, g->dacs[0], AC_PAR_PCM) : def_pcm;

    /* Everything on: paths, pins, the speaker amplifiers (EAPD), the jacks'
       messages. Volumes at 0 dB and unmuted, so the first sound is heard. */
    for (int i = 0; i < g->nouts; i++) {
        struct output *o = &g->outs[i];
        struct widget *pw = wid(g, o->pin);
        path_on(g, &o->path);
        if (pw->pincap & AC_PINCAP_EAPD)
            wr(g, o->pin, AC_VERB_SET_EAPD_BTLENABLE, AC_EAPDBTL_EAPD);
        if (o->jack && (pw->wcaps & AC_WCAP_UNSOL_CAP)) {
            wr(g, o->pin, AC_VERB_SET_UNSOLICITED_ENABLE, AC_USRSP_EN | (unsigned int)(1 + i));
            g->have_hp_jack |= o->kind == OUT_HP;
        }
    }
    for (int i = 0; i < g->nins; i++) {
        struct widget *pw = wid(g, g->ins[i].pin);
        if (g->ins[i].jack && (pw->wcaps & AC_WCAP_UNSOL_CAP)) {
            wr(g, g->ins[i].pin, AC_VERB_SET_UNSOLICITED_ENABLE, AC_USRSP_EN | (unsigned int)(16 + i));
            g->have_mic_jack = 1;
        }
    }
    struct amp *mv = g->outs[0].vol.nid ? &g->outs[0].vol : NULL;
    g->master[0] = g->master[1] = mv ? (long)(mv->caps & AC_AMPCAP_OFFSET) : 0;
    g->master_on[0] = g->master_on[1] = 1;
    g->capture[0] = g->capture[1] = g->cap_vol.nid ? (long)(g->cap_vol.caps & AC_AMPCAP_OFFSET) : 0;
    g->capture_on[0] = g->capture_on[1] = 1;
    check_jacks(g, 0);
    codec_name(g);

    kprintf("hda: %s: %d output%s (", g->chip, g->nouts, g->nouts == 1 ? "" : "s");
    for (int i = 0; i < g->nouts; i++)
        kprintf("%s%s pin 0x%02x <- DAC 0x%02x", i ? ", " : "",
                g->outs[i].kind == OUT_HP ? "headphone" : g->outs[i].kind == OUT_SPEAKER
                ? "speaker" : "line out", g->outs[i].pin,
                g->outs[i].path.nid[g->outs[i].path.n - 1]);
    kprintf("), %d input%s", g->nins, g->nins == 1 ? "" : "s");
    for (int i = 0; i < g->nins; i++)
        kprintf("%s%s mic pin 0x%02x", i ? ", " : " (", g->ins[i].internal ? "built-in" : "jack",
                g->ins[i].pin);
    kprintf("%s%s\n", g->nins ? " -> ADC" : "", g->nins ? ")" : "");
    return g;
}

const char *hda_gen_chip(struct hda_gen *g)
{
    return g->chip;
}

/* --- the streams' formats -------------------------------------------------------- */

static const unsigned int rate_hz[12] = {
    8000, 11025, 16000, 22050, 32000, 44100, 48000, 88200, 96000, 176400, 192000, 384000,
};

void hda_gen_pcm_caps(struct hda_gen *g, int stream, struct snd_pcm_hardware *hw)
{
    uint32_t pcm = stream == SNDRV_PCM_STREAM_PLAYBACK ? g->pcm_out : g->pcm_in;
    hw->nrates = 0;
    for (int b = 4; b <= 10; b++)           /* 32 kHz .. 192 kHz: the format word can say them */
        if (pcm & (1u << b) && hw->nrates < SND_RATES_MAX)
            hw->rates[hw->nrates++] = rate_hz[b];
    if (!hw->nrates)
        hw->rates[hw->nrates++] = 48000;
    hw->formats = 1ull << SNDRV_PCM_FORMAT_S16_LE;
    if (pcm & (AC_SUPPCM_BITS_24 | AC_SUPPCM_BITS_32))
        hw->formats |= 1ull << SNDRV_PCM_FORMAT_S32_LE;
    hw->msbits = pcm & AC_SUPPCM_BITS_32 ? 32 : 24;
}

int hda_gen_has_capture(struct hda_gen *g)
{
    return g->nins > 0;
}

/* HD Audio's stream format word: 48 or 44.1 kHz base, times 1-4, divided
   by 1-8; bits per sample; channels - 1. The controller and the codec's
   converters both get it. */
unsigned int hda_format(unsigned int rate, unsigned int bits, unsigned int channels,
                        unsigned int msbits)
{
    static const struct { unsigned int hz; unsigned short v; } t[] = {
        { 8000, 0x0500 }, { 11025, 0x4300 }, { 16000, 0x0200 }, { 22050, 0x4100 },
        { 32000, 0x0a00 }, { 44100, 0x4000 }, { 48000, 0x0000 }, { 88200, 0x4800 },
        { 96000, 0x0800 }, { 176400, 0x5800 }, { 192000, 0x1800 },
    };
    unsigned int v = 0;
    for (unsigned int k = 0; k < sizeof t / sizeof *t; k++)
        if (t[k].hz == rate)
            v = t[k].v;
    v |= bits == 16 ? 0x10 : msbits == 32 ? 0x40 : 0x30;   /* 16, 32 or 24 bits */
    return v | (channels - 1);
}

/* Point the converters at the stream (tag 0: stop listening). */
void hda_gen_stream(struct hda_gen *g, int stream, unsigned int tag, unsigned int fmt)
{
    if (stream == SNDRV_PCM_STREAM_PLAYBACK) {
        for (int i = 0; i < g->ndacs; i++) {
            if (tag)
                wr(g, g->dacs[i], AC_VERB_SET_STREAM_FORMAT, fmt);
            wr(g, g->dacs[i], AC_VERB_SET_CHANNEL_STREAMID, tag << 4);
        }
    } else if (g->adc) {
        if (tag)
            wr(g, g->adc, AC_VERB_SET_STREAM_FORMAT, fmt);
        wr(g, g->adc, AC_VERB_SET_CHANNEL_STREAMID, tag << 4);
    }
}

/* --- mixer controls ------------------------------------------------------------- */

enum { CTL_MASTER_VOL, CTL_MASTER_SW, CTL_CAP_VOL, CTL_CAP_SW, CTL_BOOST, CTL_HP_JACK, CTL_MIC_JACK };

static uint32_t ctl_caps(struct hda_gen *g, int which)
{
    if (which == CTL_MASTER_VOL)
        return g->outs[0].vol.caps;
    if (which == CTL_CAP_VOL)
        return g->cap_vol.caps;
    return g->ins[g->cur_in].boost.caps;
}

static int ctl_info(struct snd_kcontrol *k, struct snd_ctl_elem_info *u)
{
    struct hda_gen *g = k->private_data;
    int which = (int)k->private_value;
    u->count = which >= CTL_HP_JACK ? 1 : 2;
    if (which == CTL_MASTER_SW || which == CTL_CAP_SW || which >= CTL_HP_JACK) {
        u->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
        u->value.integer.max = 1;
    } else {
        u->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
        u->value.integer.max = (long)amp_steps(ctl_caps(g, which));
        u->value.integer.step = 1;
    }
    return 0;
}

/* Master moves every output's volume amp: the same dB where the scales
   agree (they nearly always do: one codec), else the same fraction. */
static void set_master(struct hda_gen *g)
{
    unsigned int max0 = amp_steps(g->outs[0].vol.caps);
    for (int i = 0; i < g->nouts; i++) {
        struct output *o = &g->outs[i];
        for (int ch = 0; ch < 2; ch++) {
            int on = g->master_on[ch];
            if (o->vol.nid) {
                unsigned int max = amp_steps(o->vol.caps), v = (unsigned int)g->master[ch];
                if (max != max0 && max0)
                    v = v * max / max0;
                int mute_here = !on && (!o->mute.nid || o->mute.nid == o->vol.nid);
                amp_set(g, &o->vol, ch, v, mute_here);
            }
            if (o->mute.nid && o->mute.nid != o->vol.nid)
                amp_set(g, &o->mute, ch, o->mute.caps & AC_AMPCAP_OFFSET, !on);
        }
    }
}

static void set_capture(struct hda_gen *g)
{
    if (g->cap_vol.nid) {
        struct amp a = g->cap_vol;
        if (g->nins)
            a.idx = g->ins[g->cur_in].path.idx[0];  /* the ADC's input in use */
        for (int ch = 0; ch < 2; ch++)
            amp_set(g, &a, ch, (unsigned int)g->capture[ch], !g->capture_on[ch]);
    }
    for (int i = 0; i < g->nins; i++)
        if (g->ins[i].boost.nid)
            for (int ch = 0; ch < 2; ch++)
                amp_set(g, &g->ins[i].boost, ch, (unsigned int)g->boost[ch], 0);
}

static int ctl_get(struct snd_kcontrol *k, struct snd_ctl_elem_value *v)
{
    struct hda_gen *g = k->private_data;
    for (int ch = 0; ch < 2; ch++)
        switch ((int)k->private_value) {
        case CTL_MASTER_VOL: v->value.integer[ch] = g->master[ch]; break;
        case CTL_MASTER_SW:  v->value.integer[ch] = g->master_on[ch]; break;
        case CTL_CAP_VOL:    v->value.integer[ch] = g->capture[ch]; break;
        case CTL_CAP_SW:     v->value.integer[ch] = g->capture_on[ch]; break;
        case CTL_BOOST:      v->value.integer[ch] = g->boost[ch]; break;
        case CTL_HP_JACK:    v->value.integer[0] = g->hp_in; break;
        case CTL_MIC_JACK:   v->value.integer[0] = g->mic_in; break;
        }
    return 0;
}

static int ctl_put(struct snd_kcontrol *k, struct snd_ctl_elem_value *v)
{
    struct hda_gen *g = k->private_data;
    int which = (int)k->private_value, changed = 0;
    long max = which == CTL_MASTER_SW || which == CTL_CAP_SW ? 1
             : (long)amp_steps(ctl_caps(g, which));
    long *val = which == CTL_MASTER_VOL ? g->master : which == CTL_CAP_VOL ? g->capture
              : which == CTL_BOOST ? g->boost : NULL;
    int *sw = which == CTL_MASTER_SW ? g->master_on : which == CTL_CAP_SW ? g->capture_on : NULL;
    for (int ch = 0; ch < 2; ch++) {
        long x = v->value.integer[ch];
        if (x < 0 || x > max)
            return -EINVAL;
        if (val && val[ch] != x) {
            val[ch] = x;
            changed = 1;
        }
        if (sw && sw[ch] != (int)x) {
            sw[ch] = (int)x;
            changed = 1;
        }
    }
    if (changed && (which == CTL_MASTER_VOL || which == CTL_MASTER_SW))
        set_master(g);
    else if (changed)
        set_capture(g);
    return changed;
}

static const struct snd_kcontrol_new ctls[] = {
    { SNDRV_CTL_ELEM_IFACE_MIXER, "Master Playback Volume", 0, 0, ctl_info, ctl_get, ctl_put, CTL_MASTER_VOL },
    { SNDRV_CTL_ELEM_IFACE_MIXER, "Master Playback Switch", 0, 0, ctl_info, ctl_get, ctl_put, CTL_MASTER_SW },
    { SNDRV_CTL_ELEM_IFACE_MIXER, "Capture Volume", 0, 0, ctl_info, ctl_get, ctl_put, CTL_CAP_VOL },
    { SNDRV_CTL_ELEM_IFACE_MIXER, "Capture Switch", 0, 0, ctl_info, ctl_get, ctl_put, CTL_CAP_SW },
    { SNDRV_CTL_ELEM_IFACE_MIXER, "Mic Boost Volume", 0, 0, ctl_info, ctl_get, ctl_put, CTL_BOOST },
    { SNDRV_CTL_ELEM_IFACE_CARD, "Headphone Jack", 0, SNDRV_CTL_ELEM_ACCESS_READ, ctl_info, ctl_get, NULL, CTL_HP_JACK },
    { SNDRV_CTL_ELEM_IFACE_CARD, "Mic Jack", 0, SNDRV_CTL_ELEM_ACCESS_READ, ctl_info, ctl_get, NULL, CTL_MIC_JACK },
};

static struct snd_kcontrol *add(struct hda_gen *g, int which, unsigned int tlv)
{
    struct snd_kcontrol *k = snd_ctl_new1(&ctls[which], g);
    if (!k)
        return NULL;
    if (tlv < 3) {
        db_tlv(g->dbtlv[tlv], ctl_caps(g, which));
        k->tlv = g->dbtlv[tlv];
    }
    return snd_ctl_add(g->card, k) < 0 ? NULL : k;
}

int hda_gen_add_controls(struct hda_gen *g, struct snd_card *card)
{
    g->card = card;
    if (g->outs[0].vol.nid)
        add(g, CTL_MASTER_VOL, 0);
    add(g, CTL_MASTER_SW, ~0u);
    if (g->cap_vol.nid) {
        add(g, CTL_CAP_VOL, 1);
        add(g, CTL_CAP_SW, ~0u);
    }
    if (g->nins && g->ins[g->cur_in].boost.nid)
        add(g, CTL_BOOST, 2);
    if (g->have_hp_jack)
        g->hp_jack = add(g, CTL_HP_JACK, ~0u);
    if (g->have_mic_jack)
        g->mic_jack = add(g, CTL_MIC_JACK, ~0u);
    set_master(g);
    set_capture(g);
    return 0;
}
