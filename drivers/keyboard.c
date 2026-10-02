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
 * The IRQ handler only converts the scancode and stores the character in a
 * small ring buffer; keyboard_getc() takes characters out later. Keeping
 * interrupt handlers short like this is a general rule.
 */
#include <interrupts.h>
#include <io.h>
#include <keyboard.h>

#define KBD_DATA   0x60
#define KBD_STATUS 0x64

/* Scancode -> character, without and with Shift. 0 = no character. */
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

#define SC_LSHIFT 0x2A
#define SC_RSHIFT 0x36
#define SC_CTRL   0x1D
#define SC_CAPS   0x3A
#define SC_F10    0x44
#define SC_F11    0x57
#define SC_F12    0x58

static int shift, ctrl, caps;
static int extended;            /* the previous byte was the 0xE0 prefix */

/* Once a terminal (tty.c) listens, keys go to it as the bytes a real
   terminal sends: Enter = CR, Backspace = DEL (0x7F), and the keys
   without a character as "escape sequences" (ESC [ ...), the way the
   Linux console sends them (TERM=linux). */
static void (*listener)(const char *bytes, int n);

void keyboard_set_listener(void (*fn)(const char *bytes, int n))
{
    listener = fn;
}

/* Extended keys (0xE0 prefix): scancode -> what the Linux console sends. */
static const char *extended_seq(uint8_t sc)
{
    switch (sc) {
    case 0x48: return "\033[A";        /* up */
    case 0x50: return "\033[B";        /* down */
    case 0x4D: return "\033[C";        /* right */
    case 0x4B: return "\033[D";        /* left */
    case 0x47: return "\033[1~";       /* Home */
    case 0x4F: return "\033[4~";       /* End */
    case 0x52: return "\033[2~";       /* Insert */
    case 0x53: return "\033[3~";       /* Delete */
    case 0x49: return "\033[5~";       /* Page Up */
    case 0x51: return "\033[6~";       /* Page Down */
    case 0x1C: return "\r";            /* keypad Enter */
    case 0x35: return "/";             /* keypad / */
    default:   return 0;
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

static void keyboard_irq(struct regs *r)
{
    (void)r;
    uint8_t sc = inb(KBD_DATA);

    if (sc == 0xE0) {           /* prefix: the real code comes next */
        extended = 1;
        return;
    }
    int released = sc & 0x80;
    sc &= 0x7F;

    if (extended) {             /* arrows, Home, Delete, right Ctrl... */
        extended = 0;
        if (sc == SC_CTRL)      /* right Ctrl counts as Ctrl */
            ctrl = !released;
        else if (!released && listener && extended_seq(sc))
            send(extended_seq(sc));
        return;
    }

    switch (sc) {
    case SC_LSHIFT: case SC_RSHIFT: shift = !released; return;
    case SC_CTRL:                   ctrl  = !released; return;
    }
    if (released)
        return;                 /* we only care about presses */

    if (sc == SC_CAPS)  { caps = !caps; return; }
    if (listener) {
        if (sc == SC_F10) { send("\033[21~"); return; }
        if (sc == SC_F11) { send("\033[23~"); return; }
        if (sc == SC_F12) { send("\033[24~"); return; }
    }
    if (sc == SC_F10)   { put(KEY_F10); return; }
    if (sc == SC_F11)   { put(KEY_F11); return; }
    if (sc == SC_F12)   { put(KEY_F12); return; }

    char c = shift ? shift_map[sc] : normal_map[sc];
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

void keyboard_init(void)
{
    /* Throw away anything typed before we were ready (e.g. during boot). */
    while (inb(KBD_STATUS) & 0x01)
        inb(KBD_DATA);
    irq_install(1, keyboard_irq);
}

int keyboard_getc(void)
{
    if (head == tail)
        return -1;
    int c = buf[tail];
    tail = (tail + 1) % BUF_SIZE;
    return c;
}
