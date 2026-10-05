/*
 * efi_main.c — 64-bit UEFI loader for the 32-bit MyOS kernel
 *
 * Firmware on an x64 PC (and QEMU+OVMF) starts /EFI/BOOT/BOOTX64.EFI.
 * We load the kernel and initramfs from the EFI System Partition, fill
 * the same BOOTINFO stage2.asm leaves at 0x1000, then drop to 32-bit
 * protected mode and jump to physical 1 MiB. From there the kernel is
 * the same binary the BIOS path runs.
 *
 * This is not the x86_64 port (roadmap 12b). It is the missing first
 * step: a UEFI-only machine can start the kernel we already have.
 */
#include "efi.h"

#ifndef INITRD_LOAD
#error "Makefile must pass -DINITRD_LOAD=... (first page after the kernel .bss)"
#endif
#ifndef KERNEL_LOAD
#define KERNEL_LOAD 0x100000u
#endif

#define BOOTINFO_PHYS 0x1000u
#define FONT_PHYS     0x2000u
#define JUMP_PHYS     0x3000u
#define E820_MAX      32
#define BOOTINFO_FB_MAGIC 0x3242464Du  /* 'MFB2' */

/* In sync with bootinfo.h — written as bytes so this 64-bit TU does not
   include the kernel's 32-bit headers. */
struct e820 {
    u64 base;
    u64 length;
    u32 type;
    u32 acpi;
} __attribute__((packed));

struct bootinfo {
    u32 initrd_phys;
    u32 initrd_size;
    u32 count;
    struct e820 entries[E820_MAX];
    u32 font_phys;
    u32 fb_magic;
    u32 fb_phys;
    u32 fb_pitch;
    u32 fb_width;
    u32 fb_height;
    u32 fb_bpp;
    u32 fb_format;              /* 0 = XRGB (R at bit 16), 1 = RGB (R at 0) */
    u32 fb_phys_hi;
} __attribute__((packed));

static const u8 trampoline[] = {
#include "efi_jump.inc"
};

static struct efi_system_table *g_st;

static inline void outb(u16 port, u8 v)
{
    __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port));
}
static inline u8 inb(u16 port)
{
    u8 v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static void ser(const char *s)
{
    for (; *s; s++) {
        int spins = 100000;
        while ((inb(0x3FD) & 0x20) == 0 && --spins)
            ;
        outb(0x3F8, (u8)*s);
    }
}

/* Real hardware has no serial. Say it on the firmware console too. */
static void say(const char *s)
{
    u16 buf[128];
    unsigned i = 0;

    if (!g_st || !g_st->con_out || !g_st->con_out->output_string) {
        ser(s);
        return;
    }
    while (*s && i + 2 < 127) {
        unsigned char c = (unsigned char)*s++;
        if (c == '\n') {
            buf[i++] = '\r';
            buf[i++] = '\n';
        } else {
            buf[i++] = c;
        }
    }
    buf[i] = 0;
    g_st->con_out->output_string(g_st->con_out, buf);
}

static void say_hex(u64 n)
{
    char b[19];
    int i;
    b[0] = '0'; b[1] = 'x';
    for (i = 0; i < 16; i++) {
        int d = (int)((n >> ((15 - i) * 4)) & 0xF);
        b[2 + i] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
    }
    b[18] = 0;
    say(b);
}

static u64 g_fb;
static u32 g_pitch, g_w, g_h;

static void paint_bar(u32 color)
{
    u32 y, x, rows;
    if (!g_fb || !g_pitch || !g_w || !g_h)
        return;
    rows = g_h < 48 ? g_h : 48;
    for (y = 0; y < rows; y++) {
        u32 *line = (u32 *)(g_fb + (u64)y * g_pitch);
        for (x = 0; x < g_w; x++)
            line[x] = color;
    }
}

void *memcpy(void *dst, const void *src, unsigned long n)
{
    u8 *d = dst;
    const u8 *s = src;
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memset(void *dst, int c, unsigned long n)
{
    u8 *d = dst;
    while (n--)
        *d++ = (u8)c;
    return dst;
}

static u32 pages_for(u64 bytes)
{
    return (u32)((bytes + EFI_PAGE - 1) / EFI_PAGE);
}

static int alloc_at(struct efi_boot_services *bs, u64 addr, u32 pages)
{
    efi_phys a = addr;
    return bs->allocate_pages(AllocateAddress, EfiLoaderData, pages, &a) == EFI_SUCCESS
           && a == addr;
}

static int load_file(struct efi_file *root, u16 *name, void *dest, u64 cap, u64 *out_size)
{
    struct efi_file *f = 0;
    u64 sz, pos;
    if (root->open(root, &f, name, EFI_FILE_MODE_READ, 0) != EFI_SUCCESS || !f) {
        say("open failed\r\n");
        return -1;
    }
    if (f->set_position(f, ~0ull) != EFI_SUCCESS
        || f->get_position(f, &pos) != EFI_SUCCESS) {
        f->close(f);
        return -1;
    }
    if (pos > cap) {
        say("file too big\r\n");
        f->close(f);
        return -1;
    }
    if (f->set_position(f, 0) != EFI_SUCCESS) {
        f->close(f);
        return -1;
    }
    sz = pos;
    if (f->read(f, &sz, dest) != EFI_SUCCESS || sz != pos) {
        say("read failed\r\n");
        f->close(f);
        return -1;
    }
    f->close(f);
    *out_size = pos;
    return 0;
}

static u32 efi_to_e820(u32 type)
{
    switch (type) {
    case EfiLoaderCode:
    case EfiLoaderData:
    case EfiBootServicesCode:
    case EfiBootServicesData:
    case EfiConventionalMemory:
        return 1;               /* usable */
    case EfiACPIReclaimMemory:
        return 3;
    case EfiACPIMemoryNVS:
        return 4;
    case EfiUnusableMemory:
        return 5;
    default:
        return 2;               /* reserved */
    }
}

static void fill_e820(struct bootinfo *bi, efi_memory_desc *map, u64 map_size, u64 desc_size)
{
    struct e820 tmp[96];
    u32 n = 0, i, j;
    u8 *p;

    for (p = (u8 *)map; p < (u8 *)map + map_size && n < 96; p += desc_size) {
        efi_memory_desc *d = (efi_memory_desc *)p;
        u64 len = d->pages * EFI_PAGE;
        u32 ty;
        if (!len)
            continue;
        ty = efi_to_e820(d->type);
        if (n && tmp[n - 1].type == ty
            && tmp[n - 1].base + tmp[n - 1].length == d->phys) {
            tmp[n - 1].length += len;
            continue;
        }
        tmp[n].base = d->phys;
        tmp[n].length = len;
        tmp[n].type = ty;
        tmp[n].acpi = 0;
        n++;
    }
    /* Keep the first E820_MAX after merging; drop reserved ranges above 4 GiB
       first if we overflow (the 32-bit kernel cannot use them). */
    while (n > E820_MAX) {
        int drop = -1;
        for (i = 0; i < n; i++)
            if (tmp[i].type != 1 && tmp[i].base >= 0x100000000ull) {
                drop = (int)i;
                break;
            }
        if (drop < 0)
            drop = (int)n - 1;
        for (j = (u32)drop; j + 1 < n; j++)
            tmp[j] = tmp[j + 1];
        n--;
    }
    bi->count = n;
    for (i = 0; i < n; i++)
        bi->entries[i] = tmp[i];
}

static void take_gop(struct efi_boot_services *bs, struct bootinfo *bi)
{
    struct efi_gop *gop = 0;
    struct efi_gop_mode_info *info = 0;
    efi_guid guid = gop_guid;
    u32 fmt, pixfmt = 0;
    u64 isz = 0;

    if (bs->locate_protocol(&guid, 0, (void **)&gop) != EFI_SUCCESS || !gop || !gop->mode) {
        say("no GOP\r\n");
        return;
    }
    if (gop->query_mode)
        gop->query_mode(gop, gop->mode->mode, &isz, &info);
    if (!info)
        info = gop->mode->info;
    if (!info || !gop->mode->fb_base) {
        say("GOP has no framebuffer\r\n");
        return;
    }
    fmt = info->pixel_format;
    if (fmt == PixelRedGreenBlueReserved8BitPerColor)
        pixfmt = 1;
    else if (fmt == PixelBlueGreenRedReserved8BitPerColor)
        pixfmt = 0;
    else if (fmt == PixelBitMask && info->red_mask == 0x000000FFu)
        pixfmt = 1;
    else if (fmt == PixelBitMask && info->red_mask == 0x00FF0000u)
        pixfmt = 0;
    else {
        say("GOP pixel format ");
        say_hex(fmt);
        say("\r\n");
        return;
    }
    g_fb = gop->mode->fb_base;
    g_pitch = info->pixels_per_scan_line * 4;
    g_w = info->width;
    g_h = info->height;
    say("gop ");
    say_hex(g_w);
    say("x");
    say_hex(g_h);
    say(" @ ");
    say_hex(g_fb);
    say("\r\n");
    bi->fb_magic = BOOTINFO_FB_MAGIC;
    bi->fb_phys = (u32)g_fb;
    bi->fb_phys_hi = (u32)(g_fb >> 32);
    bi->fb_pitch = g_pitch;
    bi->fb_width = g_w;
    bi->fb_height = g_h;
    bi->fb_bpp = 32;
    bi->fb_format = pixfmt;
}

efi_status efi_main(efi_handle image, struct efi_system_table *st);

/* Absolute pointers so the PE has a .reloc table. Real firmware often
   refuses to start an EFI app that can only run at one address. */
__attribute__((used))
static void *const keep_reloc[] = { (void *)efi_main };

efi_status efi_main(efi_handle image, struct efi_system_table *st)
{
    static u16 kname[] = u"\\EFI\\MYOS\\KERNEL.BIN";
    static u16 iname[] = u"\\EFI\\MYOS\\INITRD.CPIO";
    static u16 fname[] = u"\\EFI\\MYOS\\FONT.BIN";

    struct efi_boot_services *bs;
    struct efi_loaded_image *li = 0;
    struct efi_simple_fs *fs = 0;
    struct efi_file *root = 0;
    struct bootinfo *bi;
    efi_guid li_guid = loaded_image_guid, fs_guid = simple_fs_guid;
    efi_memory_desc *map = 0;
    u64 map_size = 0, map_key = 0, desc_size = 0, ksize = 0, isize = 0, fsize = 0;
    u32 desc_ver = 0, kpages, ipages;
    u64 extra, low, ksrc;
    int tries;
    efi_status stt;

    g_st = st;
    say("MyOS UEFI loader\r\n");
    if (!st || !st->boot)
        return EFI_NOT_FOUND;
    bs = st->boot;

    kpages = pages_for((u64)INITRD_LOAD - KERNEL_LOAD);
    low = BOOTINFO_PHYS;
    if (!alloc_at(bs, BOOTINFO_PHYS, 3)) {      /* bootinfo, font, trampoline */
        say("0x1000 busy, allocating elsewhere\r\n");
        low = 0xFFFFF000ull;
        if (bs->allocate_pages(AllocateMaxAddress, EfiLoaderData, 3, &low)
            != EFI_SUCCESS) {
            say("no low pages\r\n");
            return EFI_NOT_FOUND;
        }
    }
    ksrc = KERNEL_LOAD;
    if (!alloc_at(bs, KERNEL_LOAD, kpages)) {
        say("1 MiB busy, allocating elsewhere\r\n");
        ksrc = 0xFFFFF000ull;
        if (bs->allocate_pages(AllocateMaxAddress, EfiLoaderData, kpages, &ksrc)
            != EFI_SUCCESS) {
            say("no kernel pages\r\n");
            return EFI_NOT_FOUND;
        }
    }
    memset((void *)low, 0, 3 * EFI_PAGE);
    memset((void *)ksrc, 0, (u64)kpages * EFI_PAGE);

    if (bs->handle_protocol(image, &li_guid, (void **)&li) != EFI_SUCCESS || !li
        || bs->handle_protocol(li->device, &fs_guid, (void **)&fs) != EFI_SUCCESS || !fs
        || fs->open_volume(fs, &root) != EFI_SUCCESS || !root) {
        say("no ESP filesystem\r\n");
        return EFI_NOT_FOUND;
    }

    if (load_file(root, kname, (void *)ksrc,
                  (u64)INITRD_LOAD - KERNEL_LOAD, &ksize) < 0) {
        say("kernel\r\n");
        return EFI_NOT_FOUND;
    }
    say("kernel ");
    say_hex(ksize);
    say("\r\n");

    ipages = 1024;                              /* cap 4 MiB; grow if needed */
    extra = INITRD_LOAD;
    if (!alloc_at(bs, INITRD_LOAD, ipages)) {
        extra = 0xFFFFF000ull;
        if (bs->allocate_pages(AllocateMaxAddress, EfiLoaderData, ipages, &extra)
            != EFI_SUCCESS) {
            say("no room for initrd\r\n");
            return EFI_NOT_FOUND;
        }
    }
    if (extra > 0xFFFFFFFFull) {
        say("initrd above 4 GiB\r\n");
        return EFI_NOT_FOUND;
    }
    if (load_file(root, iname, (void *)extra, (u64)ipages * EFI_PAGE, &isize) < 0) {
        say("initrd\r\n");
        return EFI_NOT_FOUND;
    }
    say("initrd ");
    say_hex(isize);
    say(" @ ");
    say_hex(extra);
    say("\r\n");

    if (load_file(root, fname, (void *)(low + (FONT_PHYS - BOOTINFO_PHYS)),
                  4096, &fsize) < 0 || fsize != 4096)
        say("no font (fbcon will stay off)\r\n");

    bi = (struct bootinfo *)low;
    bi->initrd_phys = (u32)extra;
    bi->initrd_size = (u32)isize;
    bi->font_phys = (fsize == 4096) ? FONT_PHYS : 0;
    take_gop(bs, bi);

    memcpy((void *)(low + (JUMP_PHYS - BOOTINFO_PHYS)), trampoline, sizeof trampoline);

    /* Do not print to ConOut after this: OutputString allocates and the
       memory-map key ExitBootServices needs goes stale. Serial is port I/O. */
    say("calling ExitBootServices\r\n");

    /* GetMemoryMap + ExitBootServices: the map changes if we allocate, so
       the first GetMemoryMap is a size probe; then one extra page of slack. */
    for (tries = 0; tries < 4; tries++) {
        map_size = 0;
        bs->get_memory_map(&map_size, 0, &map_key, &desc_size, &desc_ver);
        map_size += desc_size * 8;
        if (bs->allocate_pool(EfiLoaderData, map_size, (void **)&map) != EFI_SUCCESS) {
            say("no map pool\r\n");
            return EFI_NOT_FOUND;
        }
        stt = bs->get_memory_map(&map_size, map, &map_key, &desc_size, &desc_ver);
        if (stt != EFI_SUCCESS) {
            bs->free_pool(map);
            map = 0;
            continue;
        }
        fill_e820(bi, map, map_size, desc_size);
        ser("e820\r\n");
        stt = bs->exit_boot_services(image, map_key);
        if (stt == EFI_SUCCESS)
            break;
        bs->free_pool(map);
        map = 0;
    }
    if (stt != EFI_SUCCESS) {
        say("ExitBootServices failed ");
        say_hex(stt);
        say("\r\n");
        return stt;
    }

    /* No firmware calls after this. Place the kernel at 1 MiB if we had
       to park it elsewhere, mark the GOP so a hang is visible, jump. */
    if (low != BOOTINFO_PHYS)
        memcpy((void *)(u64)BOOTINFO_PHYS, (void *)low, 3 * EFI_PAGE);
    if (ksrc != KERNEL_LOAD)
        memcpy((void *)(u64)KERNEL_LOAD, (void *)ksrc, (u64)kpages * EFI_PAGE);
    paint_bar(0x00C06020u);         /* golden bar: we left UEFI */
    __asm__ volatile ("cli");
    ((void (*)(void))(u64)JUMP_PHYS)();
    for (;;)
        __asm__ volatile ("hlt");
    return 0;
}
