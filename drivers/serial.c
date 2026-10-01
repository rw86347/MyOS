/*
 * serial.c — COM1 serial port output (16550 UART), interrupt-driven
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
 */
#include <interrupts.h>
#include <io.h>
#include <serial.h>

#define COM1        0x3F8
#define LSR_THRE    0x20        /* line status: transmit FIFO empty */
#define IER_THRE    0x02        /* interrupt enable: "transmit FIFO empty" */
#define FIFO_SIZE   16
#define TXBUF       8192        /* ring buffer size (power of two) */

static char txbuf[TXBUF];
static uint32_t head, tail;     /* write here / send from here; head - tail = used */
static int irq_mode;            /* 1 = interrupt-driven */
static int tx_running;          /* the THRE interrupt is on and will come */

static int fifo_empty(void)
{
    return inb(COM1 + 5) & LSR_THRE;
}

void serial_init(void)
{
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
        outb(COM1 + 1, more ? IER_THRE : 0);
        tx_running = more;
    }
}

static void serial_irq(struct regs *r)
{
    (void)r;
    inb(COM1 + 2);                      /* read IIR: acknowledges the UART */
    fill_fifo();
}

void serial_irq_init(void)
{
    irq_install(4, serial_irq);
    irq_mode = 1;
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
    return irq_mode ? TXBUF - (head - tail) : TXBUF;
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
