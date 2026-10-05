/*
 * pic.c — the 8259 Programmable Interrupt Controller
 *
 * Hardware devices (timer, keyboard, disks...) signal the CPU through
 * interrupt lines called IRQs. A PC has two 8259 PIC chips chained
 * together: the "master" handles IRQ 0-7, the "slave" IRQ 8-15 and is
 * wired into the master's IRQ 2.
 *
 *   IRQ 0 = PIT timer    IRQ 1 = keyboard    IRQ 14/15 = IDE disks
 *
 * The problem: at power-on the BIOS maps IRQ 0-7 to CPU vectors 8-15 —
 * which collide with CPU exceptions (vector 8 is "double fault"!). So we
 * reprogram ("remap") the PICs to use vectors 32-47 instead.
 *
 * After handling an IRQ we must send an "End Of Interrupt" (EOI), or the
 * PIC will never deliver that IRQ (or lower-priority ones) again.
 *
 * (Newer machines use the APIC instead; we'll switch later. Every PC still
 * has 8259-compatible PICs.)
 */
#include <io.h>
#include <pic.h>

#define PIC1_CMD   0x20     /* master */
#define PIC1_DATA  0x21
#define PIC2_CMD   0xA0     /* slave */
#define PIC2_DATA  0xA1
#define PIC_EOI    0x20
#define PIC_READ_ISR 0x0B   /* "which IRQs are being serviced right now?" */

/* Old hardware needs a moment between PIC commands. Writing to port 0x80
   (an unused POST-code port) takes about 1 µs and does nothing else. */
static void io_wait(void)
{
    outb(0x80, 0);
}

void pic_init(void)
{
    /* The initialization sequence: four "ICW" bytes to each chip. */
    outb(PIC1_CMD, 0x11);  io_wait();  /* ICW1: start init, ICW4 will follow */
    outb(PIC2_CMD, 0x11);  io_wait();
    outb(PIC1_DATA, PIC_IRQ_BASE);     io_wait();  /* ICW2: master -> vectors 32-39 */
    outb(PIC2_DATA, PIC_IRQ_BASE + 8); io_wait();  /*       slave  -> vectors 40-47 */
    outb(PIC1_DATA, 0x04); io_wait();  /* ICW3: master has a slave on IRQ 2 (bit 2) */
    outb(PIC2_DATA, 0x02); io_wait();  /*       slave's cascade identity is 2 */
    outb(PIC1_DATA, 0x01); io_wait();  /* ICW4: 8086 mode */
    outb(PIC2_DATA, 0x01); io_wait();

    /* Mask (disable) every IRQ except IRQ 2, the link to the slave chip.
       Drivers unmask their own IRQ when they're ready. */
    outb(PIC1_DATA, 0xFF & ~(1 << 2));
    outb(PIC2_DATA, 0xFF);
}

void pic_unmask(int irq)
{
    uint16_t port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    outb(port, inb(port) & ~(1 << (irq & 7)));
}

void pic_send_eoi(int irq)
{
    if (irq >= 8)
        outb(PIC2_CMD, PIC_EOI);    /* slave IRQs need an EOI to both chips */
    outb(PIC1_CMD, PIC_EOI);
}

/*
 * "Spurious" IRQs: electrical noise can make a PIC signal IRQ 7 (or 15)
 * when no device asked. Real interrupts show up in the PIC's In-Service
 * Register; spurious ones don't — and must NOT get an EOI (except that a
 * spurious IRQ 15 still needs an EOI to the master, which did see IRQ 2).
 */
int pic_is_spurious(int irq)
{
    if (irq != 7 && irq != 15)
        return 0;
    uint16_t cmd = irq == 7 ? PIC1_CMD : PIC2_CMD;
    outb(cmd, PIC_READ_ISR);
    if (inb(cmd) & 0x80)
        return 0;                   /* bit 7 set: it's real */
    if (irq == 15)
        outb(PIC1_CMD, PIC_EOI);
    return 1;
}
