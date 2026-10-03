/*
 * mouse.c — PS/2 mouse driver (IRQ 12)
 *
 * The keyboard controller (the "8042") has a second port, the AUX port,
 * where a PS/2 mouse plugs in. Both share the data port 0x60; the status
 * port 0x64 says whose byte is waiting (bit 5 = the mouse's). The mouse
 * raises IRQ 12, the keyboard IRQ 1.
 *
 * Moving or clicking makes the mouse send a PACKET:
 *
 *   byte 0   buttons (bit 0 left, 1 right, 2 middle), bit 3 always 1,
 *            bits 4/5 the sign of dx/dy, bits 6/7 "overflowed"
 *   byte 1   dx, how far right since the last packet (9-bit with the sign)
 *   byte 2   dy, how far UP (screens count down: users flip it)
 *   byte 3   wheel, only on an "IntelliMouse" (detected below)
 *
 * The interrupt handler only collects bytes and stores finished movements
 * in a small ring buffer; whoever reads the mouse (later /dev/input, for X)
 * takes them from there. Rule 2 of CLAUDE.md: handlers do the minimum.
 *
 * QEMU and UTM give the machine a PS/2 mouse (UTM: with USB off). On a real
 * PC without one, mouse_init just says so.
 */
#include <interrupts.h>
#include <io.h>
#include <kprintf.h>
#include <mouse.h>

#define DATA   0x60
#define STATUS 0x64                     /* read */
#define CMD    0x64                     /* write: a command for the 8042 */

#define ST_OUT_FULL 0x01                /* a byte waits at 0x60 for us */
#define ST_IN_FULL  0x02                /* the 8042 hasn't taken our last byte yet */
#define ST_AUX      0x20                /* ...and the waiting byte is the mouse's */

#define ACK 0xFA                        /* "done" from the mouse */
#define SPIN 100000                     /* status reads before giving up */

static int packet_size = 3;             /* 4 with a wheel */
static uint8_t packet[4];
static int got;                         /* bytes of the current packet so far */

#define RING 64                         /* movements not yet read */
static struct mouse_event ring[RING];
static volatile uint32_t head, tail;
static void (*listener)(void);

/* --- talking to the controller (only at boot, polled) ---------------------- */

static int wait_write(void)
{
    for (int i = 0; i < SPIN; i++)
        if (!(inb(STATUS) & ST_IN_FULL))
            return 0;
    return -1;
}

static int wait_read(void)
{
    for (int i = 0; i < SPIN; i++)
        if (inb(STATUS) & ST_OUT_FULL)
            return 0;
    return -1;
}

static int ctrl_cmd(uint8_t c)
{
    if (wait_write() < 0)
        return -1;
    outb(CMD, c);
    return 0;
}

/* A byte for the mouse itself (0xD4 = "the next data byte goes to AUX"),
   and its ACK. */
static int aux_send(uint8_t b)
{
    if (ctrl_cmd(0xD4) < 0 || wait_write() < 0)
        return -1;
    outb(DATA, b);
    if (wait_read() < 0)
        return -1;
    return inb(DATA) == ACK ? 0 : -1;
}

static int aux_send2(uint8_t cmd, uint8_t arg)
{
    return aux_send(cmd) < 0 ? -1 : aux_send(arg);
}

/* --- the interrupt ---------------------------------------------------------------- */

static void mouse_irq(struct regs *r)
{
    (void)r;
    uint8_t st = inb(STATUS);
    if (!(st & ST_OUT_FULL))
        return;
    uint8_t b = inb(DATA);
    if (!(st & ST_AUX))
        return;                         /* a keyboard byte (shouldn't happen here) */
    if (got == 0 && !(b & 0x08))
        return;                         /* not a first byte: lost sync, skip */
    packet[got++] = b;
    if (got < packet_size)
        return;
    got = 0;
    if (packet[0] & 0xC0)
        return;                         /* overflow: the numbers are junk */

    struct mouse_event e;
    e.buttons = packet[0] & 0x07;
    e.dx = (int16_t)(packet[1] - ((packet[0] << 4) & 0x100));   /* bit 4: dx < 0 */
    e.dy = (int16_t)(packet[2] - ((packet[0] << 3) & 0x100));   /* bit 5: dy < 0 */
    e.wheel = packet_size == 4 ? (int8_t)(packet[3] << 4) >> 4 : 0;   /* 4-bit signed */
    if (head - tail == RING)
        return;                         /* nobody reads: keep the oldest */
    ring[head % RING] = e;
    head++;
    if (listener)
        listener();
}

/* --- for the rest of the kernel ------------------------------------------------- */

int mouse_get(struct mouse_event *e)
{
    uint32_t flags = irq_save();
    int have = head != tail;
    if (have) {
        *e = ring[tail % RING];
        tail++;
    }
    irq_restore(flags);
    return have ? 0 : -1;
}

void mouse_set_listener(void (*fn)(void))
{
    listener = fn;
}

int mouse_init(void)
{
    /* Interrupts are still off: everything here is polled. */
    while (inb(STATUS) & ST_OUT_FULL)
        inb(DATA);                      /* stale bytes from the BIOS */
    if (ctrl_cmd(0xA8) < 0)             /* AUX port on */
        return -1;

    /* The controller's configuration byte: bit 1 = IRQ 12 on, bit 5 =
       AUX clock off (clear it). */
    uint8_t cfg;
    if (ctrl_cmd(0x20) < 0 || wait_read() < 0)
        return -1;
    cfg = inb(DATA);
    cfg = (cfg | 0x02) & ~0x20;
    if (ctrl_cmd(0x60) < 0 || wait_write() < 0)
        return -1;
    outb(DATA, cfg);

    if (aux_send(0xF6) < 0) {           /* defaults; no answer: no mouse */
        kprintf("mouse: no PS/2 mouse\n");
        return -1;
    }
    /* The IntelliMouse "knock": sample rates 200, 100, 80 in a row, and a
       wheel mouse answers to "who are you?" (0xF2) with 3 instead of 0. */
    uint8_t id = 0;
    if (aux_send2(0xF3, 200) == 0 && aux_send2(0xF3, 100) == 0 &&
        aux_send2(0xF3, 80) == 0 && aux_send(0xF2) == 0 && wait_read() == 0)
        id = inb(DATA);
    packet_size = id == 3 ? 4 : 3;
    aux_send2(0xF3, 100);               /* 100 packets a second at most */
    if (aux_send(0xF4) < 0) {           /* start sending movements */
        kprintf("mouse: PS/2 mouse doesn't start\n");
        return -1;
    }
    irq_install(12, mouse_irq);
    kprintf("mouse: PS/2%s\n", packet_size == 4 ? ", with a wheel" : "");
    return 0;
}
