/*
 * evdev.c — /dev/input/event0 (keyboard) and event1 (mouse)
 *
 * Linux hands keyboards and mice to programs as streams of small fixed
 * records, "input events", one per change:
 *
 *   struct input_event { struct timeval time; u16 type; u16 code; s32 value; }
 *
 *   EV_KEY  KEY_A, 1     the A key went down (0: up, 2: auto-repeat)
 *   EV_REL  REL_X, -3    the mouse moved 3 left
 *   EV_SYN  SYN_REPORT   end of one report: apply what came before together
 *
 * The X server opens these files (xf86-input-evdev) and asks with ioctls
 * what each one is: its name, and bitmaps of the event types and codes it
 * can send (a keyboard has keys; a mouse has REL_X, REL_Y and buttons).
 *
 * The keyboard and mouse interrupts call evdev_key/evdev_mouse. Each open
 * file has its own queue (a ring of QLEN events) that the interrupt
 * appends to — no allocation, a few stores per event. A full queue is
 * emptied and gets SYN_DROPPED, as on Linux, so a reader that fell behind
 * knows to re-read the key state instead of seeing half a report.
 * EVIOCGRAB gives one reader all the events (X takes the keyboard so the
 * console doesn't see the keys too).
 */
#include <errno.h>
#include <evdev.h>
#include <interrupts.h>
#include <kheap.h>
#include <mouse.h>
#include <string.h>
#include <thread.h>
#include <timer.h>
#include <uaccess.h>
#include <vfs.h>

#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_REL 0x02
#define EV_LED 0x11
#define EV_REP 0x14
#define SYN_REPORT  0
#define SYN_DROPPED 3
#define REL_X     0x00
#define REL_Y     0x01
#define REL_WHEEL 0x08
#define BTN_LEFT  0x110
#define KEY_CNT   0x300
#define BUS_I8042 0x11

#define QLEN    128                 /* events per reader */
#define READERS 4                   /* open files per device */

/* The time is a struct timeval, two longs: 16 bytes in all on i386, 24 on
   x86_64 — the same struct is right on both. */
struct input_event { long sec, usec; uint16_t type, code; int32_t value; };
_Static_assert(sizeof(struct input_event) == 2 * sizeof(long) + 8, "Linux's input_event");

struct reader {
    int dev;
    struct input_event q[QLEN];
    uint32_t head, tail;            /* events ever queued / read */
};

struct evdev {
    const char *name;
    uint16_t product;
    struct reader *readers[READERS];
    struct reader *grab;            /* EVIOCGRAB: only this one gets events */
    uint8_t ev_bits[4];             /* EV_* it sends */
    uint8_t key_bits[KEY_CNT / 8];  /* KEY_* / BTN_* it has */
    uint8_t rel_bits[2];
    uint8_t key_down[KEY_CNT / 8];  /* held now (EVIOCGKEY) */
    struct wait_queue wq;
};

static struct evdev devs[2] = {
    { .name = "AT Translated Set 2 keyboard", .product = 1 },
    { .name = "ImExPS/2 Generic Explorer Mouse", .product = 2 },
};

static void set_bit(uint8_t *bits, unsigned n) { bits[n / 8] |= 1 << (n % 8); }

static void init_bits(void)
{
    static int done;
    if (done)
        return;
    done = 1;
    struct evdev *k = &devs[0], *m = &devs[1];
    set_bit(k->ev_bits, EV_SYN), set_bit(k->ev_bits, EV_KEY);
    set_bit(k->ev_bits, EV_LED), set_bit(k->ev_bits, EV_REP);
    for (unsigned c = 1; c <= 88; c++)
        set_bit(k->key_bits, c);
    for (unsigned c = 96; c <= 111; c++)
        if (c != 101)
            set_bit(k->key_bits, c);
    for (unsigned c = 125; c <= 127; c++)
        set_bit(k->key_bits, c);
    set_bit(m->ev_bits, EV_SYN), set_bit(m->ev_bits, EV_KEY), set_bit(m->ev_bits, EV_REL);
    for (unsigned b = 0; b < 3; b++)
        set_bit(m->key_bits, BTN_LEFT + b);     /* left, right, middle */
    set_bit(m->rel_bits, REL_X), set_bit(m->rel_bits, REL_Y), set_bit(m->rel_bits, REL_WHEEL);
}

/* --- from the interrupt handlers ----------------------------------------------- */

static void put(struct reader *r, uint32_t ms, uint16_t type, uint16_t code, int32_t value)
{
    if (r->head - r->tail == QLEN) {
        r->tail = r->head;          /* fell behind: start over, and say so */
        put(r, ms, EV_SYN, SYN_DROPPED, 0);
    }
    struct input_event *e = &r->q[r->head % QLEN];
    e->sec = (long)(ms / 1000);
    e->usec = (long)(ms % 1000 * 1000);
    e->type = type, e->code = code, e->value = value;
    r->head++;
}

static void emit(struct evdev *d, uint16_t type, uint16_t code, int32_t value)
{
    uint32_t ms = timer_ms();
    if (d->grab) {
        put(d->grab, ms, type, code, value);
        return;
    }
    for (int i = 0; i < READERS; i++)
        if (d->readers[i])
            put(d->readers[i], ms, type, code, value);
}

static void report(struct evdev *d)
{
    emit(d, EV_SYN, SYN_REPORT, 0);
    wq_wake_all(&d->wq);
    wq_wake_all(&poll_wait);
}

void evdev_key(uint16_t code, int pressed)
{
    struct evdev *d = &devs[0];
    if (code >= KEY_CNT)
        return;
    uint8_t bit = 1 << (code % 8), *held = &d->key_down[code / 8];
    int value = pressed ? ((*held & bit) ? 2 : 1) : 0;    /* 2: the key repeats */
    if (!pressed && !(*held & bit))
        return;
    if (pressed)
        *held |= bit;
    else
        *held &= ~bit;
    emit(d, EV_KEY, code, value);
    report(d);
}

void evdev_mouse(const struct mouse_event *e)
{
    struct evdev *d = &devs[1];
    for (int b = 0; b < 3; b++) {
        uint16_t code = BTN_LEFT + b;
        int now = (e->buttons >> b) & 1;
        int was = (d->key_down[code / 8] >> (code % 8)) & 1;
        if (now != was) {
            d->key_down[code / 8] ^= 1 << (code % 8);
            emit(d, EV_KEY, code, now);
        }
    }
    if (e->dx)
        emit(d, EV_REL, REL_X, e->dx);
    if (e->dy)
        emit(d, EV_REL, REL_Y, -e->dy);         /* PS/2 counts up, screens down */
    if (e->wheel)
        emit(d, EV_REL, REL_WHEEL, -e->wheel);  /* as Linux's psmouse does */
    report(d);
}

/* --- the files ----------------------------------------------------------------------- */

/* The open file's reader, made on first use (open() has no hook). */
static struct reader *reader_of(struct file *f)
{
    if (f->priv)
        return f->priv;
    init_bits();
    int dev = (f->inode->rdev & 0xFF) - 64;
    struct evdev *d = &devs[dev];
    struct reader *r = kmalloc(sizeof *r);
    if (!r)
        return NULL;
    memset(r, 0, sizeof *r);
    r->dev = dev;
    uint32_t flags = irq_save();
    int slot = -1;
    for (int i = 0; i < READERS && slot < 0; i++)
        if (!d->readers[i])
            slot = i;
    if (slot >= 0)
        d->readers[slot] = r;
    irq_restore(flags);
    if (slot < 0) {
        kfree(r);
        return NULL;
    }
    f->priv = r;
    return r;
}

static int32_t ev_read(struct file *f, void *ubuf, uint32_t n)
{
    struct reader *r = reader_of(f);
    if (!r)
        return -EMFILE;
    if (n < sizeof(struct input_event))
        return -EINVAL;
    struct evdev *d = &devs[r->dev];
    while (r->head == r->tail) {
        if (f->flags & O_NONBLOCK)
            return -EAGAIN;
        if (signal_pending())
            return -EINTR;
        wq_sleep(&d->wq);
    }
    /* Take events out with interrupts off, then copy: the copy can fault
       and sleep, and the interrupt may queue more meanwhile. */
    struct input_event batch[32];
    uint32_t want = n / sizeof batch[0], k = 0;
    uint32_t flags = irq_save();
    while (k < want && k < 32 && r->tail != r->head)
        batch[k++] = r->q[r->tail++ % QLEN];
    irq_restore(flags);
    if (copy_to_user(ubuf, batch, k * sizeof batch[0]) < 0)
        return -EFAULT;
    return k * sizeof batch[0];
}

static int32_t ev_write(struct file *f, const void *ubuf, uint32_t n)
{
    (void)f, (void)ubuf;
    return n;                       /* LED changes from X: no LEDs to set */
}

static uint32_t ev_poll(struct file *f)
{
    struct reader *r = reader_of(f);
    return r && r->head != r->tail ? 0x001 : 0;     /* POLLIN */
}

static void ev_release(struct file *f)
{
    struct reader *r = f->priv;
    if (!r)
        return;
    struct evdev *d = &devs[r->dev];
    uint32_t flags = irq_save();
    for (int i = 0; i < READERS; i++)
        if (d->readers[i] == r)
            d->readers[i] = NULL;
    if (d->grab == r)
        d->grab = NULL;
    irq_restore(flags);
    kfree(r);
}

/* Copy a bitmap or string to the caller: at most len bytes. Returns the
   count, as Linux does. */
static int32_t give(uintptr_t arg, const void *src, uint32_t have, uint32_t len)
{
    uint32_t n = have < len ? have : len;
    return copy_to_user((void *)arg, src, n) < 0 ? -EFAULT : (int32_t)n;
}

static int32_t ev_ioctl(struct file *f, uint32_t cmd, uintptr_t arg)
{
    struct reader *r = reader_of(f);
    if (!r)
        return -EMFILE;
    struct evdev *d = &devs[r->dev];
    init_bits();
    uint32_t nr = cmd & 0xFF, len = (cmd >> 16) & 0x3FFF, dir = cmd >> 30;
    static const uint8_t zeros[KEY_CNT / 8];
    if (((cmd >> 8) & 0xFF) != 'E')
        return -ENOTTY;
    if (dir == 2) {                 /* the kernel writes: a question */
        if (nr >= 0x20 && nr < 0x40) {          /* EVIOCGBIT(type) */
            switch (nr - 0x20) {
            case 0:      return give(arg, d->ev_bits, sizeof d->ev_bits, len);
            case EV_KEY: return give(arg, d->key_bits, sizeof d->key_bits, len);
            case EV_REL: return give(arg, d->rel_bits, sizeof d->rel_bits, len);
            default:     return give(arg, zeros, sizeof zeros, len);
            }
        }
        if (nr >= 0x40 && nr < 0x80)
            return -EINVAL;                     /* EVIOCGABS: no absolute axes */
        switch (nr) {
        case 0x01: {                            /* EVIOCGVERSION */
            int32_t v = 0x010001;
            return give(arg, &v, 4, len) < 0 ? -EFAULT : 0;
        }
        case 0x02: {                            /* EVIOCGID */
            uint16_t id[4] = { BUS_I8042, 1, d->product, 0xAB41 };
            return give(arg, id, sizeof id, len) < 0 ? -EFAULT : 0;
        }
        case 0x03: {                            /* EVIOCGREP: delay, period (ms) */
            uint32_t rep[2] = { 250, 33 };
            if (r->dev != 0)
                return -ENOSYS;
            return give(arg, rep, sizeof rep, len) < 0 ? -EFAULT : 0;
        }
        case 0x06:                              /* EVIOCGNAME */
            return give(arg, d->name, strlen(d->name) + 1, len);
        case 0x07:                              /* EVIOCGPHYS */
            return give(arg, r->dev ? "isa0060/serio1/input0" : "isa0060/serio0/input0",
                        22, len);
        case 0x08:                              /* EVIOCGUNIQ */
            return -ENOENT;
        case 0x09:                              /* EVIOCGPROP */
        case 0x19:                              /* EVIOCGLED */
        case 0x1a:                              /* EVIOCGSND */
        case 0x1b:                              /* EVIOCGSW */
            return give(arg, zeros, nr == 0x09 ? 4 : 8, len);
        case 0x18: {                            /* EVIOCGKEY */
            uint8_t held[KEY_CNT / 8];
            uint32_t flags = irq_save();
            memcpy(held, d->key_down, sizeof held);
            irq_restore(flags);
            return give(arg, held, sizeof held, len);
        }
        }
        return -EINVAL;
    }
    switch (nr) {
    case 0x90: {                                /* EVIOCGRAB: arg is the value */
        uint32_t flags = irq_save();
        int32_t rc = 0;
        if (arg) {
            if (d->grab && d->grab != r)
                rc = -EBUSY;
            else
                d->grab = r;
        } else if (d->grab == r) {
            d->grab = NULL;
        } else {
            rc = -EINVAL;
        }
        irq_restore(flags);
        return rc;
    }
    case 0x03:                                  /* EVIOCSREP */
    case 0xa0:                                  /* EVIOCSCLOCKID */
        return 0;
    case 0x91:                                  /* EVIOCREVOKE */
        return 0;
    }
    return -EINVAL;
}

static const struct file_ops ev_ops = {
    .read = ev_read, .write = ev_write, .poll = ev_poll,
    .release = ev_release, .ioctl = ev_ioctl,
};

const struct file_ops *evdev_ops(uint32_t minor)
{
    return minor == 64 || minor == 65 ? &ev_ops : NULL;
}
