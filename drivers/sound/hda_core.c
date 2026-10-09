/*
 * hda_core.c — the HD Audio bus: talking to codecs, and finding out what
 * they are. The controller driver (hda_intel.c) moves the words; this file
 * knows what they mean. Linux's names and arguments (sound/hda/core), our
 * code, from the HD Audio spec 1.0a. docs/SOUND-PLAN.md.
 *
 * A codec is a tree: the root node (0) holds function groups (audio, maybe
 * a modem), and the audio function group holds the widgets — pins, mixers,
 * DACs, ADCs. Every node answers "parameter" questions: what it is, what
 * it can do, how many nodes are under it.
 */
#include <kheap.h>
#include <kprintf.h>
#include <sound/hdaudio.h>
#include <string.h>

unsigned int snd_hdac_make_cmd(struct hdac_device *codec, hda_nid_t nid,
                               unsigned int verb, unsigned int parm)
{
    if ((codec->addr & ~0xfu) || (nid & ~0x7fu) || (verb & ~0xfffu) || (parm & ~0xffffu))
        return ~0u;                     /* can't be said: Linux returns -1 too */
    return codec->addr << 28 | (unsigned int)nid << 20 | verb << 8 | parm;
}

int snd_hdac_codec_read(struct hdac_device *hdac, hda_nid_t nid, int flags,
                        unsigned int verb, unsigned int parm)
{
    (void)flags;
    struct hdac_bus *bus = hdac->bus;
    unsigned int cmd = snd_hdac_make_cmd(hdac, nid, verb, parm), res;
    if (cmd == ~0u)
        return -1;
    mutex_lock(&bus->cmd_lock);         /* the jack thread and ioctls share the link */
    int bad = bus->ops->command(bus, cmd) < 0 || bus->ops->get_response(bus, hdac->addr, &res) < 0;
    mutex_unlock(&bus->cmd_lock);
    return bad ? -1 : (int)res;
}

int snd_hdac_codec_write(struct hdac_device *hdac, hda_nid_t nid, int flags,
                         unsigned int verb, unsigned int parm)
{
    /* Every verb answers, if only with 0: wait for it, so the next verb
       finds the rings in step. */
    return snd_hdac_codec_read(hdac, nid, flags, verb, parm) == -1 ? -1 : 0;
}

int snd_hdac_read_parm(struct hdac_device *codec, hda_nid_t nid, int parm)
{
    return snd_hdac_codec_read(codec, nid, 0, AC_VERB_PARAMETERS, (unsigned int)parm);
}

int snd_hdac_get_sub_nodes(struct hdac_device *codec, hda_nid_t nid, hda_nid_t *start_id)
{
    int parm = snd_hdac_read_parm(codec, nid, AC_PAR_NODE_COUNT);
    if (parm == -1) {
        *start_id = 0;
        return 0;
    }
    *start_id = (hda_nid_t)((parm >> 16) & 0x7fff);
    return parm & 0x7fff;
}

/* --- what is on the link ------------------------------------------------------ */

static const char *vendor_name(uint32_t id)
{
    switch (id >> 16) {
    case 0x10ec: return "Realtek ";
    case 0x8086: return "Intel HDMI ";
    case 0x10de: return "NVIDIA HDMI ";
    case 0x1af4: return "QEMU ";
    case 0x14f1: return "Conexant ";
    case 0x1106: return "VIA ";
    case 0x11d4: return "Analog Devices ";
    case 0x1013: return "Cirrus ";
    case 0x111d: return "IDT ";
    default:     return "";
    }
}

static const char *const jack_names[16] = {
    "line out", "speaker", "headphone", "CD", "S/PDIF out", "digital out", "modem line",
    "modem handset", "line in", "aux", "mic", "telephony", "S/PDIF in", "digital in",
    "?", "other",
};

static const char *const where[16] = {
    "", "rear", "front", "left", "right", "top", "bottom", "special", "special", "special",
    "", "", "", "", "", "",
};

/* One line per connected pin: what the BIOS says it is wired to. This is
   the fact a machine's sound needs most (which pin is the speaker, which
   the jack), so the scan prints it. */
static void print_pins(struct hdac_device *c, const char *who)
{
    for (int k = 0; k < c->num_nodes; k++) {
        hda_nid_t nid = (hda_nid_t)(c->start_nid + k);
        int wcap = snd_hdac_read_parm(c, nid, AC_PAR_AUDIO_WIDGET_CAP);
        if (wcap == -1 || ((wcap >> AC_WCAP_TYPE_SHIFT) & 0xf) != AC_WID_PIN)
            continue;
        int cfg = snd_hdac_codec_read(c, nid, 0, AC_VERB_GET_CONFIG_DEFAULT, 0);
        unsigned int conn = ((unsigned int)cfg >> AC_DEFCFG_PORT_CONN_SHIFT) & 3;
        if (cfg == -1 || conn == AC_JACK_PORT_NONE)
            continue;
        unsigned int dev = ((unsigned int)cfg >> AC_DEFCFG_DEVICE_SHIFT) & 0xf;
        unsigned int loc = ((unsigned int)cfg >> AC_DEFCFG_LOCATION_SHIFT) & 0x3f;
        const char *place = conn == AC_JACK_PORT_FIXED || (loc >> 4) == 1 ? "built in"
                          : where[loc & 0xf][0] ? where[loc & 0xf] : "jack";
        kprintf("hda: %s   pin 0x%02x: %s, %s%s (config %08x)\n", who, nid, jack_names[dev],
                place, conn == AC_JACK_PORT_COMPLEX && (loc >> 4) != 1 && where[loc & 0xf][0]
                ? " jack" : "", (unsigned int)cfg);
    }
}

void snd_hdac_bus_scan(struct hdac_bus *bus, const char *who)
{
    for (unsigned int addr = 0; addr < HDA_MAX_CODECS; addr++) {
        if (!(bus->codec_mask & (1u << addr)))
            continue;
        struct hdac_device *c = kmalloc(sizeof *c);
        if (!c)
            return;
        memset(c, 0, sizeof *c);
        c->bus = bus;
        c->addr = addr;
        int v = snd_hdac_read_parm(c, AC_NODE_ROOT, AC_PAR_VENDOR_ID);
        if (v == -1) {
            kprintf("hda: %s codec %u does not answer\n", who, addr);
            kfree(c);
            continue;
        }
        c->vendor_id = (uint32_t)v;
        c->revision_id = (uint32_t)snd_hdac_read_parm(c, AC_NODE_ROOT, AC_PAR_REV_ID);
        hda_nid_t fg;
        int nfg = snd_hdac_get_sub_nodes(c, AC_NODE_ROOT, &fg);
        for (int k = 0; k < nfg; k++, fg++) {
            int type = snd_hdac_read_parm(c, fg, AC_PAR_FUNCTION_TYPE);
            if (type == -1)
                continue;
            if ((type & AC_FGT_TYPE) == AC_GRP_AUDIO_FUNCTION && !c->afg)
                c->afg = fg;
            else if ((type & AC_FGT_TYPE) == AC_GRP_MODEM_FUNCTION && !c->mfg)
                c->mfg = fg;
        }
        if (c->afg) {
            c->subsystem_id = (uint32_t)snd_hdac_codec_read(c, c->afg, 0,
                                                            AC_VERB_GET_SUBSYSTEM_ID, 0);
            c->num_nodes = snd_hdac_get_sub_nodes(c, c->afg, &c->start_nid);
        }
        bus->codecs[addr] = c;
        kprintf("hda: %s codec %u: %s%04x:%04x rev %06x, subsystem %04x:%04x, %s %d widgets\n",
                who, addr, vendor_name(c->vendor_id), (unsigned int)(c->vendor_id >> 16),
                (unsigned int)(c->vendor_id & 0xffff), (unsigned int)(c->revision_id & 0xffffff),
                (unsigned int)(c->subsystem_id >> 16), (unsigned int)(c->subsystem_id & 0xffff),
                c->afg ? "audio," : "no audio function,", c->num_nodes);
        if (c->afg)
            print_pins(c, who);
    }
}
