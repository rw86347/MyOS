/*
 * serial.c — COM1 serial port (16550 UART), interrupt-driven both ways
 *
 * The serial port is the kernel developer's best friend: it works before
 * anything else, and QEMU can show it in your terminal (-serial stdio).
 * Real old PCs have a COM1 port too, and a null-modem cable lets another
 * computer capture the output.
 *
 * COM1's registers live at I/O ports 0x3F8..0x3FF.
 *
 * Speed: at 115200 baud the port sends one byte every ~87 µs. Waiting for it
 * byte by byte would freeze the CPU for ~9 ms per 100 bytes — nine missed
 * time slots. So output goes into a ring buffer and the call returns at once;
 * the UART's "transmitter empty" interrupt (IRQ 4) feeds it 16 bytes at a
 * time (its FIFO size) in the background. Writing a byte costs a memory
 * store, not a wait.
 *
 * Before interrupts are set up (serial_irq_init), and when the kernel
 * crashes (serial_flush), we fall back to polling, so nothing is lost.
 *
 * Input: the "byte received" interrupt hands what arrives to a listener
 * (the console terminal, as the keyboard does), so a login on ttyS0 — QEMU's
 * -serial, or a null-modem cable — can be typed into.
 *
 * Most laptops have no COM1 at all. Then nothing would ever empty the ring:
 * no UART, no interrupt. It fills during boot, and from then on the console
 * refuses programs' writes (fs/dev.c waits for room in it). So serial_init
 * first checks that a UART is there, and without one serial output is off.
 */
#include <interrupts.h>
#include <io.h>
#include <serial.h>

#define COM1        0x3F8
#define SCRATCH     7           /* a spare register: keeps what we write */
#define LSR_DR      0x01        /* line status: a received byte is waiting */
#define LSR_THRE    0x20        /* line status: transmit FIFO empty */
#define IER_RDA     0x01        /* interrupt enable: "received data available" */
#define IER_THRE    0x02        /* interrupt enable: "transmit FIFO empty" */
#define FIFO_SIZE   16
#define TXBUF       8192        /* ring buffer size (power of two) */

static char txbuf[TXBUF];
static uint32_t head, tail;     /* write here / send from here; head - tail = used */
static int present;             /* a UART answers at COM1 */
static int irq_mode;            /* 1 = interrupt-driven */
static int tx_running;          /* the THRE interrupt is on and will come */
static void (*listener)(const char *bytes, int n);   /* who gets what arrives */

static int fifo_empty(void)
{
    return inb(COM1 + 5) & LSR_THRE;
}

void serial_init(void)
{
    /* A UART keeps what we write in its scratch register; with nothing at
       the port, reads give 0xFF. Two patterns, so a stuck bus can't pass. */
    outb(COM1 + SCRATCH, 0x5A);
    if (inb(COM1 + SCRATCH) != 0x5A)
        return;
    outb(COM1 + SCRATCH, 0xA5);
    if (inb(COM1 + SCRATCH) != 0xA5)
        return;
    present = 1;
    outb(COM1 + 1, 0x00);   /* no UART interrupts yet */
    outb(COM1 + 3, 0x80);   /* set DLAB bit: next two writes set the baud divisor */
    outb(COM1 + 0, 0x01);   /* divisor low byte:  115200 / 1 = 115200 baud */
    outb(COM1 + 1, 0x00);   /* divisor high byte */
    outb(COM1 + 3, 0x03);   /* 8 data bits, no parity, 1 stop bit ("8N1"); DLAB off */
    outb(COM1 + 2, 0xC7);   /* enable and clear the FIFOs */
    outb(COM1 + 4, 0x0B);   /* DTR + RTS, and OUT2: lets the UART's IRQ reach the PIC */
}

/* Move up to 16 bytes from the ring into the UART's FIFO, and keep the
   "FIFO empty" interrupt on while there's more to send. Interrupts off. */
static void fill_fifo(void)
{
    if (fifo_empty())
        for (int i = 0; i < FIFO_SIZE && tail != head; i++)
            outb(COM1, txbuf[tail++ % TXBUF]);

    int more = tail != head;
    if (more != tx_running) {           /* port write only on a change */
        outb(COM1 + 1, (more ? IER_THRE : 0) | IER_RDA);
        tx_running = more;
    }
}

/* What arrived: at most a FIFO's worth per interrupt (the UART raises the
   interrupt again while more is waiting). */
static void receive(void)
{
    char buf[FIFO_SIZE];
    int n = 0;
    while (n < FIFO_SIZE && (inb(COM1 + 5) & LSR_DR))
        buf[n++] = (char)inb(COM1);
    if (n && listener)
        listener(buf, n);
}

static void serial_irq(struct regs *r)
{
    (void)r;
    inb(COM1 + 2);                      /* read IIR: acknowledges the UART */
    receive();
    fill_fifo();
}

void serial_set_listener(void (*fn)(const char *bytes, int n))
{
    listener = fn;
}

int serial_present(void)
{
    return present;
}

/* From the timer tick. If the transmitter's interrupt never comes (a UART
   whose IRQ 4 the board doesn't wire to the 8259), keep the queue moving
   anyway: refill the FIFO when it is empty. One port read per tick, and
   only while output is waiting; with the interrupt working it finds the
   FIFO already busy. Interrupts are off (we are in the tick). */
void serial_tick(void)
{
    if (tx_running && fifo_empty())
        fill_fifo();
}

void serial_irq_init(void)
{
    if (!present)
        return;
    irq_install(4, serial_irq);
    irq_mode = 1;
    outb(COM1 + 1, IER_RDA);            /* tell us when a byte arrives */
}

/* Polling, for early boot and crashes: wait until the FIFO is empty. */
static void put_polled(char c)
{
    while (!fifo_empty())
        ;
    outb(COM1, (uint8_t)c);
}

static void put_queued(char c)
{
    if (head - tail == TXBUF) {         /* full (rare): send the oldest now */
        while (head - tail == TXBUF) {
            while (!fifo_empty())
                ;
            fill_fifo();
        }
    }
    txbuf[head++ % TXBUF] = c;
}

void serial_write_n(const char *s, size_t n)
{
    if (!present)
        return;
    if (!irq_mode) {
        for (size_t i = 0; i < n; i++) {
            if (s[i] == '\n')
                put_polled('\r');       /* terminals want CR+LF for a new line */
            put_polled(s[i]);
        }
        return;
    }
    uint32_t flags = irq_save();        /* the IRQ handler uses the ring too */
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\n')
            put_queued('\r');
        put_queued(s[i]);
    }
    if (!tx_running)
        fill_fifo();                    /* start the transmitter */
    irq_restore(flags);
}

size_t serial_space(void)
{
    return present && irq_mode ? TXBUF - (head - tail) : TXBUF;
}

void serial_flush(void)
{
    uint32_t flags = irq_save();
    while (tail != head) {
        while (!fifo_empty())
            ;
        fill_fifo();
    }
    irq_restore(flags);
}
