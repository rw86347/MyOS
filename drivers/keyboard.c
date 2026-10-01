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

    if (extended) {             /* arrows etc. — ignored for now, but right */
        extended = 0;           /* Ctrl (E0 1D) still counts as Ctrl */
        if (sc == SC_CTRL)
            ctrl = !released;
        return;
    }

    switch (sc) {
    case SC_LSHIFT: case SC_RSHIFT: shift = !released; return;
    case SC_CTRL:                   ctrl  = !released; return;
    }
    if (released)
        return;                 /* we only care about presses */

    if (sc == SC_CAPS)  { caps = !caps; return; }
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
    if (c)
        put(c);
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
