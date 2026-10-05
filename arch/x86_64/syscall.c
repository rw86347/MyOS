/*
 * syscall.c — program MSRs so musl's syscall instruction enters the kernel
 */
#include <cpu.h>
#include <gdt.h>
#include <stdint.h>

#define MSR_STAR   0xC0000081
#define MSR_LSTAR  0xC0000082
#define MSR_SFMASK 0xC0000084

extern void syscall_entry_asm(void);

void syscall_init(void)
{
    uint64_t star = ((uint64_t)(GDT_USER_CODE | 3) << 48)
                  | ((uint64_t)GDT_KERNEL_CODE << 32);
    wrmsr(MSR_STAR, star);
    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall_entry_asm);
    wrmsr(MSR_SFMASK, 1u << 9);
}

void sysenter_init(void)
{
    syscall_init();
}

uint32_t vsyscall_map(void)
{
    return 0;
}
