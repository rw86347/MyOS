/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * keyboard.c — PS/2 keyboard driver (IRQ 1)
 *
 * Each key press and release makes the keyboard controller fire IRQ 1 and
 * put a "scancode" byte in its data port (0x60). Scancodes are key
 * POSITIONS, not characters: "A" is 0x1E whatever your layout prints on it.
 * Releasing a key sends the same code with bit 7 set (0x1E -> 0x9E).
 * Some keys (arrows, right Ctrl...) send a 0xE0 prefix byte first.
 *
 * The BIOS leaves the controller translating to "scancode set 1" (the
 * original IBM PC codes), which is what the tables below use (US layout).
 *
 * The IRQ handler turns the scancode into the key's Linux code (KEY_A is
 * 30) and hands it to keyboard_key, which every keyboard uses (USB ones
 * too: usb_hid.c). keyboard_key gives the key to /dev/input/event0 and to
 * the terminal as characters, or stores the character in a small ring
 * buffer that keyboard_getc() takes from later. Keeping interrupt
 * handlers short like this is a general rule.
 */
#include <evdev.h>
#include <interrupts.h>
#include <io.h>
#include <keyboard.h>
#include <kprintf.h>
#include <power.h>
#include <suspend.h>
#include <thread.h>

#define KBD_DATA   0x60
#define KBD_STATUS 0x64

/* Key -> character, without and with Shift. 0 = no character. The index
   is the Linux key code, which for these keys is the set-1 scancode. */
static const char normal_map[128] = {
    0, 27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',       /* 0x00-0x0E */
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',         /* 0x0F-0x1C */
    0, 'a','s','d','f','g','h','j','k','l',';','\'','`',               /* 0x1D-0x29 */
    0, '\\','z','x','c','v','b','n','m',',','.','/', 0,                /* 0x2A-0x36 */
    '*', 0, ' ',                                                       /* 0x37-0x39 */
};
static const char shift_map[128] = {
    0, 27, '!','@','#','$','%','^','&','*','(',')','_','+','\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0, 'A','S','D','F','G','H','J','K','L',':','"','~',
    0, '|','Z','X','C','V','B','N','M','<','>','?', 0,
    '*', 0, ' ',
};

/* The Linux key codes (input-event-codes.h) the terminal side looks at. */
#define K_T       20
#define K_LCTRL   29
#define K_LSHIFT  42
#define K_RSHIFT  54
#define K_LALT    56
#define K_CAPS    58
#define K_F10     68
#define K_KPDOT   83          /* keypad Del */
#define K_F11     87
#define K_F12     88
#define K_RCTRL   97
#define K_SYSRQ   99          /* Print Screen; SysRq with Alt */
#define K_RALT    100
#define K_DELETE  111

static int shift, ctrl, alt, caps;
static int extended;            /* the previous byte was the 0xE0 prefix */
static int tty_on = 1;          /* 0: keys go only to /dev/input (an X server
                                   set KDSKBMODE K_OFF on the console) */

void keyboard_set_tty(int on)
{
    tty_on = on;
}

/* The key's Linux code (KEY_*, input-event-codes.h). For set-1 scancodes
   below 0x59 it IS the scancode (but 0x54, what Alt+Print Screen sends, is
   SysRq); the 0xE0 keys have codes of their own. 0: none (the fake Shifts
   around Print Screen). */
static uint16_t linux_key(uint8_t sc, int ext)
{
    if (!ext)
        return sc == 0x54 ? K_SYSRQ : sc < 0x59 ? sc : 0;
    switch (sc) {
    case 0x1C: return 96;       /* keypad Enter */
    case 0x1D: return 97;       /* right Ctrl */
    case 0x35: return 98;       /* keypad / */
    case 0x37: return 99;       /* Print Screen */
    case 0x38: return 100;      /* right Alt */
    case 0x47: return 102;      /* Home */
    case 0x48: return 103;      /* up */
    case 0x49: return 104;      /* Page Up */
    case 0x4B: return 105;      /* left */
    case 0x4D: return 106;      /* right */
    case 0x4F: return 107;      /* End */
    case 0x50: return 108;      /* down */
    case 0x51: return 109;      /* Page Down */
    case 0x52: return 110;      /* Insert */
    case 0x53: return 111;      /* Delete */
    case 0x5B: return 125;      /* left Windows key */
    case 0x5C: return 126;      /* right Windows key */
    case 0x5D: return 127;      /* Menu */
    default:   return 0;
    }
}

/* Once a terminal (tty.c) listens, keys go to it as the bytes a real
   terminal sends: Enter = CR, Backspace = DEL (0x7F), and the keys
   without a character as "escape sequences" (ESC [ ...), the way the
   Linux console sends them (TERM=linux). */
static void (*listener)(const char *bytes, int n);

void keyboard_set_listener(void (*fn)(const char *bytes, int n))
{
    listener = fn;
}

/* The keys without a character in the maps: what the Linux console
   sends for them. */
static const char *special_seq(uint16_t key)
{
    switch (key) {
    case 103:   return "\033[A";       /* up */
    case 108:   return "\033[B";       /* down */
    case 106:   return "\033[C";       /* right */
    case 105:   return "\033[D";       /* left */
    case 102:   return "\033[1~";      /* Home */
    case 107:   return "\033[4~";      /* End */
    case 110:   return "\033[2~";      /* Insert */
    case 111:   return "\033[3~";      /* Delete */
    case 104:   return "\033[5~";      /* Page Up */
    case 109:   return "\033[6~";      /* Page Down */
    case 96:    return "\r";           /* keypad Enter */
    case 98:    return "/";            /* keypad / */
    case K_F10: return "\033[21~";
    case K_F11: return "\033[23~";
    case K_F12: return "\033[24~";
    default:    return 0;
    }
}

static void send(const char *s)
{
    int n = 0;
    while (s[n])
        n++;
    listener(s, n);
}

/* Ring buffer: the IRQ handler writes at head, keyboard_getc reads at tail.
   One writer and one reader on one CPU, so no lock is needed. */
#define BUF_SIZE 64
static volatile int buf[BUF_SIZE];
static volatile unsigned head, tail;

static void put(int c)
{
    unsigned next = (head + 1) % BUF_SIZE;
    if (next != tail) {         /* if the buffer is full, drop the key */
        buf[head] = c;
        head = next;
    }
}

void keyboard_key(uint16_t key, int pressed)
{
    /* Alt+SysRq+T, as on Linux: every thread and what it waits for
       (thread_dump), even while the X server has the keyboard — the way
       to see what a hung system is doing. */
    static int sysrq_alt, sysrq;
    if (key == K_LALT || key == K_RALT)
        sysrq_alt = pressed;
    else if (key == K_SYSRQ)
        sysrq = pressed;
    else if (key == K_T && pressed && sysrq && sysrq_alt)
        thread_dump();
    if (suspend_asleep && pressed) {
        suspend_input();                /* asleep: it wakes the machine, it isn't typed */
        return;
    }
    evdev_key(key, pressed);
    if (!tty_on)                /* the X server has the keyboard */
        return;

    switch (key) {
    case K_LSHIFT: case K_RSHIFT: shift = pressed; return;
    case K_LCTRL:  case K_RCTRL:  ctrl  = pressed; return;    /* either Ctrl, */
    case K_LALT:   case K_RALT:   alt   = pressed; return;    /* either Alt */
    }
    if (!pressed)
        return;                 /* we only care about presses */

    if (key == K_CAPS) { caps = !caps; return; }
    if ((key == K_DELETE || key == K_KPDOT) && ctrl && alt) { power_ctrl_alt_del(); return; }
    if (listener && special_seq(key)) { send(special_seq(key)); return; }
    if (key == K_F10)   { put(KEY_F10); return; }
    if (key == K_F11)   { put(KEY_F11); return; }
    if (key == K_F12)   { put(KEY_F12); return; }
    if (key >= sizeof normal_map)
        return;

    char c = shift ? shift_map[key] : normal_map[key];
    if (caps && c >= 'a' && c <= 'z')
        c -= 'a' - 'A';
    else if (caps && c >= 'A' && c <= 'Z')
        c += 'a' - 'A';         /* Caps Lock + Shift gives lowercase */
    if (ctrl && c >= 'a' && c <= 'z')
        c = c - 'a' + 1;        /* Ctrl-A = 1 ... Ctrl-Z = 26, like a terminal */
    else if (ctrl && c >= 'A' && c <= 'Z')
        c = c - 'A' + 1;
    if (!c)
        return;
    if (listener) {             /* a terminal: send what a real one would */
        if (c == '\n')
            c = '\r';           /* Enter */
        else if (c == '\b')
            c = 0x7F;           /* Backspace */
        listener(&c, 1);
    } else {
        put(c);
    }
}

static void keyboard_irq(struct regs *r)
{
    (void)r;
    uint8_t st = inb(KBD_STATUS);
    if (!(st & 0x01) || (st & 0x20))
        return;                 /* nothing, or the mouse's byte (mouse.c) */
    uint8_t sc = inb(KBD_DATA);

    if (sc == 0xE0) {           /* prefix: the real code comes next */
        extended = 1;
        return;
    }
    uint16_t key = linux_key(sc & 0x7F, extended);
    extended = 0;
    if (key)
        keyboard_key(key, !(sc & 0x80));    /* bit 7: released */
}

int keyboard_init(void)
{
    /* Throw away anything typed before we were ready (e.g. during boot).
       A PC without the controller (most laptops since about 2015: their
       keyboards are USB or I2C) reads 0xFF from every port, which looks
       like a byte that is always waiting: after 16, as Linux's i8042_flush
       decides, there is no controller, and IRQ 1 is left alone. */
    int n = 0;
    while ((inb(KBD_STATUS) & 0x01) && n++ < 16)
        inb(KBD_DATA);
    if (n > 16) {
        klog("keyboard: no PS/2 controller\n");
        return -1;
    }
    irq_install(1, keyboard_irq);
    return 0;
}

int keyboard_getc(void)
{
    if (head == tail)
        return -1;
    int c = buf[tail];
    tail = (tail + 1) % BUF_SIZE;
    return c;
}
