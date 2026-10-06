/*
 * apic.c — the local APIC, for MSI and MSI-X
 *
 * Every x86 CPU since the Pentium has a "local APIC": its own interrupt
 * controller, a page of registers at 0xFEE00000 (or, in x2APIC mode, a
 * range of MSRs). On a PC the 8259 PIC's output still reaches the CPU
 * through it, on its LINT0 pin ("virtual wire" mode); that is how the
 * timer and keyboard get to us, and we leave it exactly as the firmware
 * set it.
 *
 * A modern PCI device needs no interrupt line at all. With MSI it signals
 * by WRITING a small message — 4 bytes of data to an address in the
 * 0xFEE00000 page. The chipset turns that write into an interrupt at the
 * CPU named in the address, with the vector named in the data. NVMe, xHCI
 * and Wi-Fi cards want this; on the MSI Katana they have no line the 8259
 * could see at all ("irq 255").
 *
 * IRQs 16-31 are ours to hand out as MSI vectors 48-63 (interrupts.c).
 * The APIC is switched on the first time a driver asks for one, so a PC
 * whose devices all use the 8259 runs exactly as before.
 */
#include <apic.h>
#include <cpu.h>
#include <interrupts.h>
#include <linux/io.h>
#include <pic.h>

#define MSR_APIC_BASE  0x1B
#define BASE_X2APIC    (1u << 10)       /* registers are MSRs 0x800 + offset/16 */
#define BASE_ENABLE    (1u << 11)       /* switched on at all (the BIOS may not) */

#define R_ID           0x020
#define R_VERSION      0x030            /* bits 23:16: the last LVT entry */
#define R_TPR          0x080            /* task priority: 0 = take everything */
#define R_EOI          0x0B0
#define R_SVR          0x0F0            /* spurious vector + software enable */
#define R_LVT_CMCI     0x2F0
#define R_LVT_TIMER    0x320
#define R_LVT_THERMAL  0x330
#define R_LVT_PERF     0x340
#define R_LVT_ERROR    0x370

#define SVR_ENABLE     (1u << 8)
#define SPURIOUS       0xFF             /* low 4 bits must be 1 on old APICs */
#define LVT_MASKED     (1u << 16)

static volatile uint32_t *regs;        /* the register page (xAPIC mode) */
static int x2apic, ready, next_irq = IRQ_MSI_FIRST;

static uint32_t rd(uint32_t r)
{
    return x2apic ? (uint32_t)rdmsr(0x800 + r / 16) : regs[r / 4];
}

static void wr(uint32_t r, uint32_t v)
{
    if (x2apic)
        wrmsr(0x800 + r / 16, v);
    else
        regs[r / 4] = v;
}

int lapic_enable(void)
{
    if (ready)
        return 0;
    if (!cpu_has(CPU_APIC) || !cpu_has(CPU_MSR))
        return -1;
    uint64_t base = rdmsr(MSR_APIC_BASE);
    if (!(base & BASE_ENABLE))
        return -1;                      /* off (some old laptops' BIOS): 8259 only */
    x2apic = (base & BASE_X2APIC) != 0;
    if (!x2apic && !(regs = ioremap((uint32_t)base & 0xFFFFF000u, 4096)))
        return -1;

    uint32_t flags = irq_save();
    /* Silence what we don't use (whatever the firmware left there): its
       timer, and the entries this APIC has (the version register says
       how many). LINT0/LINT1 — the 8259 and NMI — stay as they are. */
    uint32_t maxlvt = (rd(R_VERSION) >> 16) & 0xFF;
    wr(R_LVT_TIMER, LVT_MASKED);
    if (maxlvt >= 3)
        wr(R_LVT_ERROR, LVT_MASKED);
    if (maxlvt >= 4)
        wr(R_LVT_PERF, LVT_MASKED);
    if (maxlvt >= 5)
        wr(R_LVT_THERMAL, LVT_MASKED);
    if (maxlvt >= 6)
        wr(R_LVT_CMCI, LVT_MASKED);
    wr(R_TPR, 0);
    wr(R_SVR, rd(R_SVR) | SVR_ENABLE | SPURIOUS);
    ready = 1;
    irq_restore(flags);
    return 0;
}

void lapic_eoi(void)
{
    wr(R_EOI, 0);
}

int msi_irq_alloc(int n)
{
    if (n <= 0 || next_irq + n > NR_IRQS)
        return -1;
    int first = next_irq;
    next_irq += n;
    return first;
}

void msi_compose(int irq, uint32_t *addr_lo, uint32_t *addr_hi, uint32_t *data)
{
    /* This CPU's APIC id: the top byte of R_ID, or all of it in x2APIC
       mode. Physical destination, fixed delivery, edge-triggered. */
    uint32_t id = x2apic ? rd(R_ID) : rd(R_ID) >> 24;
    *addr_lo = 0xFEE00000u | (id & 0xFF) << 12;
    *addr_hi = 0;
    *data = (uint32_t)(PIC_IRQ_BASE + irq);
}
