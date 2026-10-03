/*
 * gdt.c — the kernel's Global Descriptor Table
 *
 * The MBR set up a temporary GDT inside its own 512 bytes (at 0x7C00),
 * memory we will want to reuse later. So the kernel builds its own.
 *
 * Reminder (see mbr.asm for the bit-level layout): in protected mode a
 * segment register holds a SELECTOR = index * 8 + RPL, where RPL is the
 * privilege level (0 = kernel, 3 = user). We use the "flat" model: every
 * segment covers all 4 GiB starting at 0. Segments then only matter for
 * one thing: the privilege level code runs at.
 *
 *   index  selector  what
 *   0      0x00      null (required, unusable)
 *   1      0x08      kernel code, ring 0
 *   2      0x10      kernel data, ring 0
 *   3      0x1B      user code,   ring 3   (0x18 | RPL 3) — used from step 5
 *   4      0x23      user data,   ring 3   (0x20 | RPL 3)
 *   5      0x28      TSS (Task State Segment)
 *   6-8    0x33...   thread-local storage (TLS): set per thread by the
 *                    set_thread_area syscall; musl points GS at entry 6
 *
 * The TSS: when an interrupt or `int 0x80` arrives while a USER program
 * runs, the CPU switches to ring 0 — and must not keep using the program's
 * stack (it could point anywhere). So it loads a kernel stack from the TSS:
 * SS0:ESP0. We give every thread its own kernel stack and update ESP0 on
 * every thread switch (tss_set_kernel_stack). The TSS can do much more
 * (hardware task switching) — nobody uses that, Linux included.
 */
#include <stdint.h>
#include <gdt.h>
#include <string.h>

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;        /* present, privilege level, code/data, permissions */
    uint8_t  flags_limit;   /* high 4 bits: flags; low 4 bits: limit bits 16-19 */
    uint8_t  base_high;
} __attribute__((packed));

/* The 6-byte operand of the LGDT instruction. */
struct gdt_ptr {
    uint16_t size;          /* table size in bytes, minus 1 */
    uint32_t base;          /* address of the table */
} __attribute__((packed));

/* The 32-bit TSS. We only use ss0/esp0 and iomap_base. */
struct tss {
    uint32_t prev_task;
    uint32_t esp0, ss0;             /* stack for entering ring 0 */
    uint32_t esp1, ss1, esp2, ss2;
    uint32_t cr3, eip, eflags, eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs, ldt;
    uint16_t trap, iomap_base;
} __attribute__((packed));

static struct gdt_entry gdt[9];
/* Not static: sysentry.asm reads esp0 from it directly. */
struct tss kernel_tss;

static void set_entry(int i, uint32_t base, uint32_t limit, uint8_t access, uint8_t flags)
{
    gdt[i].limit_low   = limit & 0xFFFF;
    gdt[i].base_low    = base & 0xFFFF;
    gdt[i].base_mid    = (base >> 16) & 0xFF;
    gdt[i].access      = access;
    gdt[i].flags_limit = (flags << 4) | ((limit >> 16) & 0x0F);
    gdt[i].base_high   = (base >> 24) & 0xFF;
}

void gdt_init(void)
{
    /* access byte: 0x9A = present | ring 0 | code | executable + readable
                    0x92 = present | ring 0 | data | writable
                    0xFA / 0xF2 = the same, but ring 3 (bits 5-6 = 11)
       flags 0xC: 4 KiB granularity (limit counts pages) + 32-bit segment */
    set_entry(0, 0, 0, 0, 0);
    set_entry(1, 0, 0xFFFFF, 0x9A, 0xC);
    set_entry(2, 0, 0xFFFFF, 0x92, 0xC);
    set_entry(3, 0, 0xFFFFF, 0xFA, 0xC);
    set_entry(4, 0, 0xFFFFF, 0xF2, 0xC);

    /* TSS descriptor: access 0x89 = present | ring 0 | "32-bit TSS, not busy".
       iomap_base past the end = no I/O permission bitmap, so user programs
       can't use in/out on any port. */
    kernel_tss.ss0 = GDT_KERNEL_DATA;
    kernel_tss.iomap_base = sizeof kernel_tss;
    set_entry(5, (uint32_t)(uintptr_t)&kernel_tss, sizeof kernel_tss - 1, 0x89, 0x0);

    struct gdt_ptr ptr = { sizeof gdt - 1, (uint32_t)(uintptr_t)gdt };

    /* Load the new table, then reload every segment register so the CPU
       re-reads the descriptors. CS can only be changed by a far jump. */
    __asm__ volatile (
        "lgdt %0\n\t"
        "ljmp %1, $1f\n"            /* far jump to the label "1:" just below */
        "1:\n\t"
        "mov %2, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "mov %%ax, %%ss\n\t"
        : : "m"(ptr), "i"(GDT_KERNEL_CODE), "i"(GDT_KERNEL_DATA) : "eax", "memory");

    /* Tell the CPU which GDT entry is the TSS ("load task register"). */
    __asm__ volatile ("ltr %w0" : : "r"(GDT_TSS));
}

void tss_set_kernel_stack(uint32_t esp0)
{
    kernel_tss.esp0 = esp0;
}

/* --- thread-local storage -------------------------------------------------- */

/* Every thread can have up to 3 segments of its own (entries 6-8), which
   the C library uses to find its per-thread data (errno, ...) through GS.
   On a thread switch we copy the new thread's three descriptors in; the
   next `pop gs` on the way back to user mode loads the new base. */
void gdt_load_tls(const uint64_t tls[GDT_TLS_ENTRIES])
{
    memcpy(&gdt[GDT_TLS_FIRST], tls, GDT_TLS_ENTRIES * sizeof(uint64_t));
}

/* Whether `pop fs` / `pop gs` on the way out of the kernel can load this
   selector without faulting. This needs a look at the descriptor and not just
   at the selector's low bits, because those pops run at CPL 0: the CPU then
   requires DPL >= 3, so 0x0B (the kernel code segment with ring-3 bits set)
   or an index past the end of the table raises #GP *inside the kernel*, which
   this kernel treats as fatal. Since a program writes its own signal frame,
   that would be one rt_sigreturn away from halting the machine.

   A thread's TLS entries are only descriptors once set_thread_area has filled
   them in, so an unset one is absent and correctly refused here; checking the
   live table rather than a list of indices is what gets that right. */
int gdt_user_seg_ok(uint32_t sel)
{
    uint32_t idx = sel >> 3;
    if ((sel & 7) != 3 || idx >= sizeof gdt / sizeof gdt[0])
        return 0;                   /* in the LDT, not ring 3, or off the end */
    uint8_t a = gdt[idx].access;
    if ((a & 0xF0) != 0xF0)         /* present, ring 3, and not a system segment */
        return 0;
    if ((a & 0x08) && !(a & 0x02))  /* a code segment that cannot be read */
        return 0;
    return 1;
}

/* struct user_desc (the set_thread_area argument) -> a descriptor. */
uint64_t gdt_tls_descriptor(uint32_t base, uint32_t limit, uint32_t flags)
{
    /* flags: bit 0 seg_32bit, bits 1-2 contents, 3 read_exec_only,
              4 limit_in_pages, 5 seg_not_present, 6 useable */
    int seg32 = flags & 1, contents = (flags >> 1) & 3, ro = (flags >> 3) & 1;
    int pages = (flags >> 4) & 1, absent = (flags >> 5) & 1, avl = (flags >> 6) & 1;
    if (base == 0 && limit == 0 && ro && absent)
        return 0;                       /* "clear this entry" */

    uint8_t access = (absent ? 0 : 0x80) | 0x60 | 0x10 | 0x01;  /* ring 3, data/code */
    if (contents == 2)
        access |= 0x08 | (ro ? 0 : 0x02);       /* code; readable */
    else
        access |= (contents == 1 ? 0x04 : 0) | (ro ? 0 : 0x02);   /* data; writable */
    uint8_t f = (pages << 3) | (seg32 << 2) | avl;

    struct gdt_entry e = {
        .limit_low = limit & 0xFFFF, .base_low = base & 0xFFFF,
        .base_mid = (base >> 16) & 0xFF, .access = access,
        .flags_limit = (uint8_t)((f << 4) | ((limit >> 16) & 0x0F)),
        .base_high = (base >> 24) & 0xFF,
    };
    uint64_t d;
    memcpy(&d, &e, sizeof d);
    return d;
}
