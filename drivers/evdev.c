/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * evdev.c — /dev/input/event0 (keyboard), event1 (mouse), event2
 * (touch screens and tablets: absolute pointers), event3 (the lid) and
 * event4 (the power button)
 *
 * Linux hands keyboards and mice to programs as streams of small fixed
 * records, "input events", one per change:
 *
 *   struct input_event { struct timeval time; u16 type; u16 code; s32 value; }
 *
 *   EV_KEY  KEY_A, 1     the A key went down (0: up, 2: auto-repeat)
 *   EV_REL  REL_X, -3    the mouse moved 3 left
 *   EV_ABS  ABS_X, 900   a finger (or a tablet's pen) is at x = 900
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
 *
 * The lid and the power button come from ACPI (acpi_button.c) as Linux's
 * "Lid Switch" (EV_SW SW_LID, 1: shut; EVIOCGSW says how it is now) and
 * "Power Button" (KEY_POWER, or KEY_WAKEUP when it woke the machine).
 * X doesn't open them; the power daemon does. Asleep (kernel/suspend.c), a
 * click wakes the machine and is not passed on (keyboard.c does the same
 * for keys); a button let go still is, so none stays held down.
 */
#include <errno.h>
#include <evdev.h>
#include <interrupts.h>
#include <kheap.h>
#include <mouse.h>
#include <string.h>
#include <suspend.h>
#include <thread.h>
#include <timer.h>
#include <uaccess.h>
#include <vfs.h>

#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_REL 0x02
#define EV_ABS 0x03
#define EV_SW  0x05
#define EV_LED 0x11
#define EV_REP 0x14
#define SYN_REPORT  0
#define SYN_DROPPED 3
#define REL_X     0x00
#define REL_Y     0x01
#define REL_WHEEL 0x08
#define ABS_X     0x00
#define ABS_Y     0x01
#define BTN_LEFT  0x110
#define KEY_CNT   0x300
#define BUS_USB   0x03
#define BUS_I8042 0x11
#define BUS_HOST  0x19

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
    uint8_t revoked;                /* EVIOCREVOKE: cut off for good */
};

struct evdev {
    const char *name, *phys;
    uint16_t bus, product;
    struct reader *readers[READERS];
    struct reader *grab;            /* EVIOCGRAB: only this one gets events */
    uint8_t ev_bits[4];             /* EV_* it sends */
    uint8_t key_bits[KEY_CNT / 8];  /* KEY_* / BTN_* it has */
    uint8_t rel_bits[2];
    uint8_t abs_bits[8];            /* ABS_* it has (event2: X and Y) */
    int32_t abs[2];                 /* where X and Y are now (EVIOCGABS) */
    uint8_t key_down[KEY_CNT / 8];  /* held now (EVIOCGKEY) */
    uint8_t sw_bits[1], sw_on[1];   /* SW_* it has, and which are on now (EVIOCGSW) */
    struct wait_queue wq;
};

#define NDEVS EVDEV_COUNT
static struct evdev devs[NDEVS] = {
    { .name = "AT Translated Set 2 keyboard", .phys = "isa0060/serio0/input0",
      .bus = BUS_I8042, .product = 1 },
    { .name = "ImExPS/2 Generic Explorer Mouse", .phys = "isa0060/serio1/input0",
      .bus = BUS_I8042, .product = 2 },
    /* Every absolute pointer, in one range (EVDEV_ABS_MAX): X maps it
       onto the screen. A touch is the left button: X's evdev driver takes
       a device with BTN_TOUCH *and* buttons for a touchpad, which moves
       the pointer only while a finger is down (UTM's tablet never says
       one is). With the buttons alone it is a touch screen: it points. */
    { .name = "MyOS touch screen", .phys = "usb-myos/input0", .bus = BUS_USB, .product = 3 },
    { .name = "Lid Switch", .phys = "PNP0C0D/button/input0", .bus = BUS_HOST, .product = 5 },
    { .name = "Power Button", .phys = "LNXPWRBN/button/input0", .bus = BUS_HOST, .product = 1 },
};

static void set_bit(uint8_t *bits, unsigned n) { bits[n / 8] |= 1 << (n % 8); }

static void init_bits(void)
{
    static int done;
    if (done)
        return;
    done = 1;
    struct evdev *k = &devs[0], *m = &devs[1], *t = &devs[2], *l = &devs[3], *p = &devs[4];
    set_bit(k->ev_bits, EV_SYN), set_bit(k->ev_bits, EV_KEY);
    set_bit(k->ev_bits, EV_LED), set_bit(k->ev_bits, EV_REP);
    for (unsigned c = 1; c <= 88; c++)
        set_bit(k->key_bits, c);
    for (unsigned c = 96; c <= 111; c++)
        if (c != 101)
            set_bit(k->key_bits, c);
    for (unsigned c = 125; c <= 127; c++)
        set_bit(k->key_bits, c);
    /* F13-F24: USB keyboards', and a touchpad's swipes (HID_SWIPE_*, F14-F17).
       Readers through libevdev (X, libinput) drop keys that aren't listed. */
    for (unsigned c = 183; c <= 194; c++)
        set_bit(k->key_bits, c);
    set_bit(m->ev_bits, EV_SYN), set_bit(m->ev_bits, EV_KEY), set_bit(m->ev_bits, EV_REL);
    for (unsigned b = 0; b < 3; b++)
        set_bit(m->key_bits, BTN_LEFT + b);     /* left, right, middle */
    set_bit(m->rel_bits, REL_X), set_bit(m->rel_bits, REL_Y), set_bit(m->rel_bits, REL_WHEEL);
    set_bit(t->ev_bits, EV_SYN), set_bit(t->ev_bits, EV_KEY);
    set_bit(t->ev_bits, EV_ABS), set_bit(t->ev_bits, EV_REL);
    for (unsigned b = 0; b < 3; b++)
        set_bit(t->key_bits, BTN_LEFT + b);     /* a tablet's (or QEMU's) buttons; a touch */
    set_bit(t->abs_bits, ABS_X), set_bit(t->abs_bits, ABS_Y);
    set_bit(t->rel_bits, REL_WHEEL);
    set_bit(l->ev_bits, EV_SYN), set_bit(l->ev_bits, EV_SW), set_bit(l->sw_bits, EVDEV_SW_LID);
    set_bit(p->ev_bits, EV_SYN), set_bit(p->ev_bits, EV_KEY);
    set_bit(p->key_bits, EVDEV_KEY_POWER), set_bit(p->key_bits, EVDEV_KEY_WAKEUP);
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
    struct mouse_event up;
    if (suspend_asleep) {               /* a click wakes; only buttons let go pass */
        suspend_input();
        up = (struct mouse_event){ .buttons = e->buttons & (d->key_down[BTN_LEFT / 8] & 7) };
        e = &up;
    }
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

/* A button (key) on device d is held or not: an event only if that changed. */
static void button(struct evdev *d, uint16_t code, int now)
{
    int was = (d->key_down[code / 8] >> (code % 8)) & 1;
    if (now != was) {
        d->key_down[code / 8] ^= 1 << (code % 8);
        emit(d, EV_KEY, code, now);
    }
}

void evdev_touch(const struct touch_event *e)
{
    struct evdev *d = &devs[2];
    struct touch_event up;
    if (suspend_asleep) {               /* as for the mouse */
        suspend_input();
        up = (struct touch_event){ .x = d->abs[0], .y = d->abs[1],
                                   .buttons = (e->buttons | (e->touch > 0)) &
                                              (d->key_down[BTN_LEFT / 8] & 7) };
        e = &up;
    }
    if (e->x != d->abs[0])
        emit(d, EV_ABS, ABS_X, d->abs[0] = e->x);
    if (e->y != d->abs[1])
        emit(d, EV_ABS, ABS_Y, d->abs[1] = e->y);
    for (int b = 0; b < 3; b++)            /* a finger or pen down is a left click */
        button(d, BTN_LEFT + b, ((e->buttons >> b) & 1) || (b == 0 && e->touch > 0));
    if (e->wheel)
        emit(d, EV_REL, REL_WHEEL, e->wheel);
    report(d);
}

void evdev_switch(uint16_t code, int on)
{
    struct evdev *d = &devs[3];
    uint32_t f = irq_save();
    if (((d->sw_on[0] >> code) & 1) != !!on) {
        d->sw_on[0] ^= (uint8_t)(1 << code);
        emit(d, EV_SW, code, !!on);
        report(d);
    }
    irq_restore(f);
}

void evdev_power(uint16_t code)
{
    struct evdev *d = &devs[4];
    uint32_t f = irq_save();
    emit(d, EV_KEY, code, 1);
    report(d);
    emit(d, EV_KEY, code, 0);
    report(d);
    irq_restore(f);
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
        if (r->revoked)
            return -ENODEV;
        if (f->flags & O_NONBLOCK)
            return -EAGAIN;
        if (signal_pending())
            return -ERESTARTSYS;
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
    if (r && r->revoked)
        return 0x018;                               /* POLLERR | POLLHUP */
    return r && r->head != r->tail ? 0x001 : 0;     /* POLLIN */
}

/* r gets no more events (interrupts off). */
static void detach(struct evdev *d, struct reader *r)
{
    for (int i = 0; i < READERS; i++)
        if (d->readers[i] == r)
            d->readers[i] = NULL;
    if (d->grab == r)
        d->grab = NULL;
}

static void ev_release(struct file *f)
{
    struct reader *r = f->priv;
    if (!r)
        return;
    uint32_t flags = irq_save();
    detach(&devs[r->dev], r);
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
    if (r->revoked)
        return -ENODEV;
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
            case EV_ABS: return give(arg, d->abs_bits, sizeof d->abs_bits, len);
            case EV_SW:  return give(arg, d->sw_bits, sizeof d->sw_bits, len);
            default:     return give(arg, zeros, sizeof zeros, len);
            }
        }
        if (nr >= 0x40 && nr < 0x80) {          /* EVIOCGABS(axis): its range */
            uint32_t axis = nr - 0x40;
            if (r->dev != 2 || axis > ABS_Y)
                return -EINVAL;
            /* struct input_absinfo: value, minimum, maximum, fuzz, flat, resolution */
            int32_t info[6] = { d->abs[axis], 0, EVDEV_ABS_MAX, 0, 0, 0 };
            return give(arg, info, sizeof info, len) < 0 ? -EFAULT : 0;
        }
        switch (nr) {
        case 0x01: {                            /* EVIOCGVERSION */
            int32_t v = 0x010001;
            return give(arg, &v, 4, len) < 0 ? -EFAULT : 0;
        }
        case 0x02: {                            /* EVIOCGID */
            uint16_t id[4] = { d->bus, 1, d->product, 0xAB41 };
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
            return give(arg, d->phys, strlen(d->phys) + 1, len);
        case 0x08:                              /* EVIOCGUNIQ */
            return -ENOENT;
        case 0x09:                              /* EVIOCGPROP */
        case 0x19:                              /* EVIOCGLED */
        case 0x1a:                              /* EVIOCGSND */
            return give(arg, zeros, nr == 0x09 ? 4 : 8, len);
        case 0x1b: {                            /* EVIOCGSW: the lid, shut or not */
            uint8_t on[8] = { d->sw_on[0] };
            return give(arg, on, sizeof on, len);
        }
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
    case 0x91: {                                /* EVIOCREVOKE */
        /* seatd cuts a session off from the keyboard when its terminal
           is switched away: no more events, and every later call fails. */
        if (arg)
            return -EINVAL;
        uint32_t flags = irq_save();
        detach(d, r);
        r->revoked = 1;
        irq_restore(flags);
        wq_wake_all(&d->wq);
        wq_wake_all(&poll_wait);
        return 0;
    }
    }
    return -EINVAL;
}

static const struct file_ops ev_ops = {
    .read = ev_read, .write = ev_write, .poll = ev_poll,
    .release = ev_release, .ioctl = ev_ioctl,
};

/* --- /sys/devices/virtual/input ------------------------------------------------- */

static char *put_str(char *s, const char *t)
{
    while (*t)
        *s++ = *t++;
    *s = 0;
    return s;
}

static char *put_hex(char *s, unsigned long v)
{
    char b[2 * sizeof v];
    int i = 0;
    do
        b[i++] = "0123456789abcdef"[v & 15];
    while (v >>= 4);
    while (i)
        *s++ = b[--i];
    *s = 0;
    return s;
}

/* A bitmap as Linux shows it in sysfs and uevents: words the size of a
   long, the highest first, in hex, space-separated, leading zero words
   left out ("120013"). Userspace reads it back in longs of its own size —
   ours, since the kernel is built for the same width. */
static char *put_bitmap(char *s, const uint8_t *bits, uint32_t bytes)
{
    const uint32_t wb = sizeof(unsigned long);
    int top = (int)((bytes + wb - 1) / wb) - 1;
    unsigned long w[KEY_CNT / 8 / sizeof(unsigned long)];
    for (int i = 0; i <= top; i++) {
        w[i] = 0;
        for (uint32_t b = 0; b < wb && i * wb + b < bytes; b++)
            w[i] |= (unsigned long)bits[i * wb + b] << (8 * b);
    }
    while (top > 0 && !w[top])
        top--;
    for (int i = top; i >= 0; i--) {
        s = put_hex(s, w[i]);
        if (i)
            *s++ = ' ';
    }
    *s = 0;
    return s;
}

static int has(const uint8_t *bits, unsigned n) { return bits[n / 8] >> (n % 8) & 1; }

void evdev_uevent(int n, char *out)
{
    struct evdev *d = &devs[n];
    init_bits();
    char *s = put_str(out, "PRODUCT=");
    s = put_hex(s, d->bus);
    s = put_str(s, "/1/");
    s = put_hex(s, d->product);
    s = put_str(put_str(put_str(put_str(s, "/ab41\nNAME=\""), d->name), "\"\nPHYS=\""), d->phys);
    s = put_str(s, "\"\nPROP=0\nEV=");
    s = put_bitmap(s, d->ev_bits, sizeof d->ev_bits);
    if (has(d->ev_bits, EV_KEY))
        s = put_bitmap(put_str(s, "\nKEY="), d->key_bits, sizeof d->key_bits);
    if (has(d->ev_bits, EV_REL))
        s = put_bitmap(put_str(s, "\nREL="), d->rel_bits, sizeof d->rel_bits);
    if (has(d->ev_bits, EV_ABS))
        s = put_bitmap(put_str(s, "\nABS="), d->abs_bits, sizeof d->abs_bits);
    if (has(d->ev_bits, EV_SW))
        s = put_bitmap(put_str(s, "\nSW="), d->sw_bits, sizeof d->sw_bits);
    if (has(d->ev_bits, EV_LED))
        s = put_str(s, "\nLED=0");
    put_str(s, "\n");
}

void evdev_node_uevent(int n, char *out)
{
    /* udev's tags, which libinput sorts devices by. libudev-zero works
       them out from the bitmaps too, but not for event2: absolute X/Y
       plus a wheel is an absolute mouse to udev (QEMU's tablet is tagged
       so on Linux) and nothing to libudev-zero. */
    static const char *const tags[NDEVS] = {
        "ID_INPUT=1\nID_INPUT_KEY=1\nID_INPUT_KEYBOARD=1\n",
        "ID_INPUT=1\nID_INPUT_MOUSE=1\n",
        "ID_INPUT=1\nID_INPUT_MOUSE=1\n",
        "ID_INPUT=1\nID_INPUT_SWITCH=1\n",
        "ID_INPUT=1\nID_INPUT_KEY=1\n",
    };
    char *s = put_str(out, "MAJOR=13\nMINOR=6");
    *s++ = (char)('4' + n);
    s = put_str(s, "\nDEVNAME=input/event");
    *s++ = (char)('0' + n);
    put_str(put_str(s, "\n"), tags[n]);
}

const struct file_ops *evdev_ops(uint32_t minor)
{
    return minor >= 64 && minor < 64 + NDEVS ? &ev_ops : NULL;
}
