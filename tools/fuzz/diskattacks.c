/* diskattacks.c — crafted disk images that must not break the kernel.
 *
 * The companion to tools/fuzz/attacks.c, which does this for the network
 * stack. Each case starts from a real ext2 image made by mke2fs, changes the
 * one field an attacker would change, and runs the kernel's filesystem code
 * over the result in a fresh child process. The case passes if the child
 * exits cleanly: AddressSanitizer turns an out-of-bounds access into a
 * non-zero exit, and the harness turns an information leak into one too.
 *
 * Running each case in its own process is what makes a failure legible. The
 * caches and the VFS keep static state and there are no reset functions for
 * them, so sharing a process would mean one case inheriting another's
 * mounted filesystem. It also means a case that corrupts memory cannot take
 * the rest of the suite down with it.
 *
 * `make verify-disktests` reintroduces each of the fixes these cases are
 * about and checks that the matching case notices. A case that passes
 * against the broken code is measuring nothing — which is exactly what
 * happened twice while the network checks were being written.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "diskstubs.h"

void disk_exercise(const uint8_t *image, uint32_t bytes);

/* --- the base image ------------------------------------------------------ */

static uint8_t *base;
static uint32_t base_n;

static void load_base(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "diskattacks: cannot open %s\n", path);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 4 * 1024 * 1024) {
        fprintf(stderr, "diskattacks: %s is %ld bytes, want 1 KiB - 4 MiB\n", path, n);
        exit(2);
    }
    base = malloc((size_t)n);
    base_n = (uint32_t)n;
    if (!base || fread(base, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "diskattacks: cannot read %s\n", path);
        exit(2);
    }
    fclose(f);
}

/* ext2's superblock is at byte 1024 whatever the block size is. */
#define SB 1024
static uint8_t *work;

static void start_from_base(void)
{
    memcpy(work, base, base_n);
}

static void put16_at(uint8_t *p, uint32_t off, uint16_t v)
{
    p[off] = v & 0xFF;
    p[off + 1] = (uint8_t)(v >> 8);
}

static void put32_at(uint8_t *p, uint32_t off, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[off + i] = (uint8_t)(v >> (8 * i));
}

static void put16(uint32_t off, uint16_t v)
{
    work[off] = v & 0xFF;
    work[off + 1] = (uint8_t)(v >> 8);
}

static void put32(uint32_t off, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        work[off + i] = (uint8_t)(v >> (8 * i));
}

static uint32_t get32(uint32_t off)
{
    return (uint32_t)work[off] | (uint32_t)work[off + 1] << 8 |
           (uint32_t)work[off + 2] << 16 | (uint32_t)work[off + 3] << 24;
}

static uint32_t get16v(uint32_t off)
{
    return (uint32_t)work[off] | (uint32_t)work[off + 1] << 8;
}

/* Superblock field offsets, from the ext2 layout. */
#define SB_INODES_COUNT   (SB + 0)
#define SB_BLOCKS_COUNT   (SB + 4)
#define SB_FIRST_DATA     (SB + 20)
#define SB_LOG_BSIZE      (SB + 24)
#define SB_BPG            (SB + 32)
#define SB_IPG            (SB + 40)
#define SB_REV            (SB + 76)
#define SB_INODE_SIZE     (SB + 88)

/* Offsets within an inode, and within a directory entry. */
#define I_MODE   0
#define I_SIZE   4
#define I_BLOCKS 28
#define I_BLOCK0 40
#define D_INODE  0
#define D_RECLEN 4
#define D_NAMELEN 6
#define D_FTYPE  7

/* Read once from the pristine image, so that a case which corrupts the
   superblock does not also move the places this file writes to. */
static uint32_t b_size, b_isize, b_itable, b_ibitmap, b_root_blk, b_sub_blk;

static uint32_t bsize(void)      { return b_size; }
static uint32_t first_data(void) { return get32(SB_FIRST_DATA); }

/* The offset in the image of inode number ino. */
static uint32_t inode_off(uint32_t ino)
{
    return b_itable * b_size + (ino - 1) * b_isize;
}

/* Find a name in a directory block; returns the offset of its entry, or 0. */
static uint32_t find_entry(uint32_t blk, const char *name, uint32_t nlen)
{
    for (uint32_t off = 0; off + 8 <= b_size; ) {
        uint32_t rl = get16v(blk + off + D_RECLEN);
        if (rl < 8 || off + rl > b_size)
            break;
        if (get32(blk + off + D_INODE) && work[blk + off + D_NAMELEN] == nlen &&
            memcmp(work + blk + off + 8, name, nlen) == 0)
            return blk + off;
        off += rl;
    }
    return 0;
}

static void learn_layout(void)
{
    memcpy(work, base, base_n);
    b_size   = 1024u << get32(SB_LOG_BSIZE);
    b_isize  = get32(SB_REV) ? get16v(SB_INODE_SIZE) : 128;
    if (!b_isize)
        b_isize = 128;
    uint32_t gd = (first_data() + 1) * b_size;
    b_ibitmap = get32(gd + 4) * b_size;
    b_itable  = get32(gd + 8);
    b_root_blk = get32(inode_off(2) + I_BLOCK0) * b_size;

    uint32_t e = find_entry(b_root_blk, "sub", 3);
    b_sub_blk = e ? get32(inode_off(get32(e + D_INODE)) + I_BLOCK0) * b_size : 0;

    if (!b_root_blk || !b_sub_blk) {
        fprintf(stderr, "diskattacks: cannot find the base image's directories\n");
        exit(2);
    }
}

/* --- running one case ---------------------------------------------------- */

static int passed, failed;

static void ok(const char *name, const char *why)
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "diskattacks: fork failed\n");
        exit(2);
    }
    if (pid == 0) {
        /* The child's stderr is kept: a sanitizer report is the explanation. */
        disk_exercise(work, base_n);
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    int clean = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (clean) {
        printf("  pass  %s\n", name);
        passed++;
    } else {
        if (WIFSIGNALED(status))
            printf("  FAIL  %s (killed by signal %d)\n", name, WTERMSIG(status));
        else
            printf("  FAIL  %s (exit %d)\n", name, WEXITSTATUS(status));
        printf("        %s\n", why);
        failed++;
    }
}

/* --- the cases ----------------------------------------------------------- */

static void test_clean(void)
{
    printf("A sound image still works\n");
    start_from_base();
    ok("an image made by mke2fs mounts, walks and takes writes",
       "the harness or a check is too strict: this image is valid");
}

static void test_superblock(void)
{
    printf("\nSuperblock fields\n");

    /* blocks_count of zero: the mount message divides by it. libgcc's 64-bit
       divide raises the hardware trap on a zero divisor, which is a #DE in
       ring 0 -- the machine is dead at boot, before userspace exists. */
    start_from_base();
    put32(SB_BLOCKS_COUNT, 0);
    ok("blocks_count of zero is refused",
       "a zero block count divided by zero at mount: #DE in ring 0, dead at boot");

    /* Under first_data_block, fs->groups underflows to a huge number. */
    start_from_base();
    put32(SB_BLOCKS_COUNT, first_data());
    ok("a block count at or below first_data_block is refused",
       "the group count underflowed to about four billion");

    /* blocks_per_group of zero: fs->groups divides by it. Found by the fuzzer
       after the check for it was already written -- it sat three lines below
       the division it was meant to guard. */
    start_from_base();
    put32(SB_BPG, 0);
    ok("blocks_per_group of zero is refused",
       "the group count divided by zero: #DE in ring 0, dead at boot");

    /* inode_size must be a power of two, or an inode straddles the end of
       its block: copy_attrs then reads the next page, and ext2_create
       memsets into it. */
    start_from_base();
    put32(SB_REV, 1);
    put16(SB_INODE_SIZE, 132);
    ok("an inode_size that is not a power of two is refused",
       "an inode straddled the end of its block: reads and a memset past the buffer");

    start_from_base();
    put32(SB_REV, 1);
    put16(SB_INODE_SIZE, 3000);
    ok("an inode_size larger than it should be is refused",
       "an oversized inode_size let a memset run past the block buffer");

    /* One bitmap is one block, so a group cannot hold more inodes than that
       block has bits: take_bit scans off the end of the page and sets a bit
       in whatever follows. */
    /* One bitmap is one block, so a group cannot hold more inodes than that
       block has bits. The bitmap is also filled in, because take_bit stops at
       the first free bit: with every bit set it scans the whole page and then
       keeps going, and the dword it finally writes to is outside it. */
    start_from_base();
    put32(SB_IPG, bsize() * 8 + 1);
    memset(work + b_ibitmap, 0xFF, bsize());
    ok("inodes_per_group past the bits in a bitmap block is refused",
       "take_bit scanned past the bitmap page and set a bit outside it");

    start_from_base();
    put32(SB_IPG, 0x01000000);
    memset(work + b_ibitmap, 0xFF, bsize());
    ok("a very large inodes_per_group is refused",
       "a huge inodes_per_group was accepted and the bitmap walked out of bounds");

    start_from_base();
    put32(SB_BPG, bsize() * 8 + 1);
    ok("blocks_per_group past the bits in a bitmap block is refused",
       "the block bitmap was scanned and written past the end of its page");

    /* first_data_block can only be 0 or 1 (1 for 1 KiB blocks). */
    start_from_base();
    put32(SB_FIRST_DATA, 0x40000000);
    ok("an impossible first_data_block is refused",
       "a first_data_block far past the disk was accepted");

    start_from_base();
    put32(SB_INODES_COUNT, 0);
    ok("an inode count of zero is refused",
       "a filesystem with no inodes was mounted");
}

static void test_dirents(void)
{
    printf("\nDirectory entries\n");

    uint32_t dir = b_root_blk, b = bsize();

    /* rec_len of 0 would walk the same entry forever. */
    start_from_base();
    put16(dir + D_RECLEN, 0);
    ok("a directory entry with rec_len 0 does not loop",
       "a zero rec_len walked the same entry forever");

    start_from_base();
    put16(dir + D_RECLEN, (uint16_t)(b + 64));
    ok("a rec_len past the end of the block is rejected",
       "an entry claiming to extend past its block was walked");

    /* An odd rec_len puts the next entry at an odd address, where reading its
       uint16_t fields is undefined C. Harmless on x86, which is why reading
       the code never flagged it; the Raspberry Pi port is where it would
       have mattered. The fuzzer found this one. */
    start_from_base();
    put16(dir + D_RECLEN, 9);
    ok("a rec_len that is not a multiple of four is rejected",
       "the next entry was read at an odd address: undefined, and a fault on ARM");

    /* name_len longer than the record, with the entry at the very end of the
       block so the read leaves the page rather than staying inside it. The
       last entry's rec_len is shortened to make room for ours. bcache gives
       each buffer a whole page and a block here is a whole page, so the byte
       after the block is in a redzone. */
    start_from_base();
    uint32_t last = find_entry(dir, "sub", 3);
    put16(last + D_RECLEN, (uint16_t)(b - (last - dir) - 8));
    put32(dir + b - 8 + D_INODE, 12);
    put16(dir + b - 8 + D_RECLEN, 8);
    work[dir + b - 8 + D_NAMELEN] = 255;        /* no room for a byte of it */
    work[dir + b - 8 + D_FTYPE] = 1;
    ok("a name_len longer than its record cannot read the next page",
       "255 bytes were read past the end of the block and returned as a filename");

    /* The same shape, but with plenty of room in the block, so the read is
       in bounds and the only question is whether the name is believed. */
    start_from_base();
    put16(dir + D_RECLEN, 12);
    work[dir + D_NAMELEN] = 255;
    ok("a name_len longer than its record is rejected",
       "a name was read from outside the record that declared it");

    /* "."'s rec_len decides where ".." is written when a directory is moved
       to a new parent, and it comes off the disk. Unchecked it is a four-byte
       write at any offset up to 64 KiB past a one-page buffer, with a value
       also taken from the disk. exercise_writes renames /sub, so /sub's own
       first block is the one that has to be corrupted. */
    start_from_base();
    put16(b_sub_blk + D_RECLEN, (uint16_t)(b + 16));
    ok("a rec_len on \".\" just past the block cannot place the \"..\" write",
       "the \"..\" fixup wrote four bytes past the end of the block buffer");

    /* rec_len is 16 bits, so the real reach is 64 KiB. A modest overflow is
       what gets probed above, because the guard is `at + 8 <= bsize` and so
       the first byte past the block is the whole question -- and because a
       60000-byte jump clears the redzone after this page and can land in
       another live allocation, where nothing would notice. */
    start_from_base();
    put16(b_sub_blk + D_RECLEN, 0xFFFF);
    ok("the largest rec_len \".\" can hold is refused too",
       "a rec_len of 65535 was used as the offset of \"..\"");

    start_from_base();
    put16(b_sub_blk + D_RECLEN, (uint16_t)(b - 4));
    ok("a \".\" rec_len leaving no room for \"..\" is handled",
       "the \"..\" fixup wrote past the end of the block buffer");

    /* Well inside the block, but at an odd offset, which makes the fixup's
       store a misaligned four-byte write. The bounds check alone let this
       through; the fuzz corpus found it after the walk had been fixed. */
    start_from_base();
    put16(b_sub_blk + D_RECLEN, 13);
    ok("an odd \".\" rec_len does not place \"..\" at an odd address",
       "the \"..\" fixup did a misaligned 4-byte write: a fault on ARM");
}

static void test_inodes(void)
{
    printf("\nInodes and symlinks\n");

    uint32_t ft = find_entry(b_root_blk, "file.txt", 8);
    uint32_t ino = get32(ft + D_INODE);

    /* A fast symlink's target is the 60 bytes of block[], whatever size says.
       Reporting more hands the caller the rest of a kmalloc'd buffer that
       nobody wrote -- which no sanitizer can see, because the memory is
       validly allocated, so the harness fills fresh allocations with a
       pattern and looks for it coming back out. */
    start_from_base();
    put16(inode_off(ino) + I_MODE, 0xA000 | 0777);
    put32(inode_off(ino) + I_SIZE, 200);
    put32(inode_off(ino) + I_BLOCKS, 0);            /* so it is "fast" */
    ok("a fast symlink cannot report more than the 60 bytes it holds",
       "readlink returned bytes past the target that were never written");

    start_from_base();
    put16(inode_off(ino) + I_MODE, 0xA000 | 0777);
    put32(inode_off(ino) + I_SIZE, 255);
    put32(inode_off(ino) + I_BLOCKS, 0);
    ok("a fast symlink claiming almost PATH_MAX is still only 60 bytes",
       "readlink returned most of a buffer that was never written");

    /* A symlink whose target block is a hole: load_symlink fails, and if its
       failure is ignored the inode keeps a size with a NULL target, so the
       first path that walks through it dereferences NULL in ring 0. */
    start_from_base();
    put16(inode_off(ino) + I_MODE, 0xA000 | 0777);
    put32(inode_off(ino) + I_SIZE, 40);
    put32(inode_off(ino) + I_BLOCKS, 8);            /* not "fast" */
    put32(inode_off(ino) + I_BLOCK0, 0);            /* and a hole */
    ok("a symlink whose target cannot be read has no size left over",
       "an unreadable symlink kept its size with a NULL target: NULL deref in ring 0");

    /* inode_size must divide the block size, or an inode straddles the end of
       its block. 132 is not a power of two: inode 32 lands at byte 4092 of a
       4096-byte block, so the 128 bytes read from it cross the page. The
       entry is pointed at inode 32 to make the walk read it. */
    start_from_base();
    put16(SB_INODE_SIZE, 132);
    put32(ft + D_INODE, 32);
    ok("an inode_size that does not divide the block size is refused",
       "an inode straddled the end of its block and the read left the page");

    start_from_base();
    put16(SB_INODE_SIZE, 3000);
    put32(ft + D_INODE, 2);
    ok("an inode_size larger than it should be is refused",
       "an oversized inode_size let a read or a memset run past the block buffer");
}

static void test_junk(void)
{
    printf("\nImages that are not filesystems\n");

    start_from_base();
    memset(work, 0, base_n);
    ok("an image of zeros is refused", "a zeroed image was mounted");

    start_from_base();
    memset(work, 0xFF, base_n);
    ok("an image of 0xFF is refused", "an image of all ones was mounted");

    /* The magic is right but nothing else is. */
    start_from_base();
    memset(work + SB, 0xFF, 512);
    put16(SB + 56, 0xEF53);
    ok("the ext2 magic alone is not enough to mount",
       "an image with the right magic and nothing else was mounted");

    /* Valid superblock, group descriptors pointing off the disk. */
    start_from_base();
    uint32_t gd = (first_data() + 1) * bsize();
    put32(gd + 0, 0x7FFFFFFF);                  /* bg_block_bitmap */
    put32(gd + 4, 0x7FFFFFFF);                  /* bg_inode_bitmap */
    put32(gd + 8, 0x7FFFFFFF);                  /* bg_inode_table */
    ok("group descriptors pointing off the disk are survivable",
       "a group descriptor past the end of the device was followed");

    /* An inode table that overlaps the superblock: writes would eat it. */
    start_from_base();
    put32(gd + 8, 0);
    ok("an inode table at block 0 is survivable",
       "an inode table on top of the boot block was used");
}

/* --- the fuzz loop -------------------------------------------------------
 *
 * Mutating a filesystem image at random is mostly wasted effort: almost every
 * change makes the superblock fail its checks and the run stops at the first
 * branch. Two things make it work anyway. The mutations are aimed at the
 * structures that matter -- the superblock, the group descriptors, the inode
 * table and the directory blocks -- rather than spread evenly over a megabyte
 * of mostly-zero data. And an image that reaches a branch nothing has reached
 * before is kept and mutated again, so progress accumulates instead of being
 * thrown away each time.
 */
#define COV_SIZE  (1u << 17)
#define CORPUS_MAX 256

static uint8_t *cov_shared;      /* the child marks this */
static uint8_t *cov_seen;        /* what the corpus has covered so far */

struct entry { uint8_t *img; uint32_t n; };
static struct entry corpus[CORPUS_MAX];
static int corpus_n;

static uint64_t rng_s = 0x2545F4914F6CDD1Dull;

static uint32_t rnd(uint32_t n)
{
    rng_s ^= rng_s << 13;
    rng_s ^= rng_s >> 7;
    rng_s ^= rng_s << 17;
    return n ? (uint32_t)(rng_s >> 33) % n : 0;
}

/* Offsets worth changing: the superblock, the group descriptor table, the
   start of the inode table, and the two directory blocks we know about. */
static uint32_t interesting_off(void)
{
    uint32_t b = bsize();
    switch (rnd(6)) {
    case 0: return SB + rnd(256);
    case 1: return (first_data() + 1) * b + rnd(64);
    case 2: return b_itable * b + rnd(16 * b_isize);
    case 3: return b_root_blk + rnd(b);
    case 4: return b_sub_blk + rnd(b);
    default: return rnd(base_n);
    }
}

static void mutate(uint8_t *img, uint32_t n)
{
    int rounds = 1 + (int)rnd(6);
    for (int i = 0; i < rounds; i++) {
        uint32_t off = interesting_off();
        if (off + 4 > n)
            off = n - 4;
        switch (rnd(4)) {
        case 0: img[off] = (uint8_t)rnd(256); break;
        case 1: put16_at(img, off, (uint16_t)rnd(65536)); break;
        case 2: put32_at(img, off, rnd(16) == 0 ? 0 : (uint32_t)(rng_s >> 11)); break;
        default: {
            /* A value right at a boundary is worth more than a random one. */
            static const uint32_t edge[] = { 0, 1, 0xFF, 0x100, 0xFFFF, 0x10000,
                                             0x7FFFFFFF, 0x80000000, 0xFFFFFFFF };
            put32_at(img, off, edge[rnd(sizeof edge / sizeof edge[0])]);
            break;
        }
        }
    }
}

/* Run one image in a child and say whether it crashed. New coverage is left
   in cov_shared for the caller to compare. */
static int run_child(const uint8_t *img, uint32_t n)
{
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "diskfuzz: fork failed\n");
        exit(2);
    }
    if (pid == 0) {
        cov_use(cov_shared, COV_SIZE);
        disk_exercise(img, n);
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return !(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static int cov_new(void)
{
    int found = 0;
    for (uint32_t i = 0; i < COV_SIZE; i++)
        if (cov_shared[i] && !cov_seen[i]) {
            cov_seen[i] = 1;
            found++;
        }
    return found;
}

static uint32_t cov_count(void)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < COV_SIZE; i++)
        n += cov_seen[i] != 0;
    return n;
}

/* An image is 4 MiB, so keeping one per corpus entry would be hundreds of
   megabytes of binary in the repository. Every entry is a mutation of the same
   base image and differs from it in a few dozen bytes, so what gets written is
   the difference: a magic line, then (offset, length, bytes) runs. The result
   is a few hundred bytes, and `git show` on one tells you what the case does.
   The base image is generated by the Makefile and not committed, which is why
   it is rebuilt from mke2fs rather than stored. */
#define DELTA_MAGIC "MYOSDISKDELTA1\n"

static int save_delta(const char *path, const uint8_t *img, uint32_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    fwrite(DELTA_MAGIC, 1, sizeof DELTA_MAGIC - 1, f);
    fwrite(&n, 4, 1, f);
    for (uint32_t i = 0; i < n; ) {
        if (i < base_n && img[i] == base[i]) { i++; continue; }
        uint32_t start = i;
        while (i < n && !(i < base_n && img[i] == base[i]))
            i++;
        uint32_t len = i - start;
        fwrite(&start, 4, 1, f);
        fwrite(&len, 4, 1, f);
        fwrite(img + start, 1, len, f);
    }
    return fclose(f) ? -1 : 0;
}

/* Rebuild an image from the base plus a delta, or load it whole if it is not
   one. Returns the length, or 0 if the file cannot be used. */
static uint32_t load_delta(const char *path, uint8_t **out)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    char magic[sizeof DELTA_MAGIC - 1];
    uint32_t n = 0;
    if (fread(magic, 1, sizeof magic, f) != sizeof magic ||
        memcmp(magic, DELTA_MAGIC, sizeof magic) || fread(&n, 4, 1, f) != 1 ||
        n == 0 || n > 64u * 1024 * 1024) {
        fclose(f);
        return 0;                       /* not a delta: the caller loads it raw */
    }
    uint8_t *img = malloc(n);
    if (!img) {
        fclose(f);
        return 0;
    }
    memcpy(img, base, n < base_n ? n : base_n);
    if (n > base_n)
        memset(img + base_n, 0, n - base_n);
    uint32_t off, len;
    while (fread(&off, 4, 1, f) == 1 && fread(&len, 4, 1, f) == 1) {
        if (off > n || len > n - off || fread(img + off, 1, len, f) != len) {
            fprintf(stderr, "diskattacks: %s is a truncated delta\n", path);
            fclose(f);
            free(img);
            return 0;
        }
    }
    fclose(f);
    *out = img;
    return n;
}

static void save_crash(const uint8_t *img, uint32_t n, uint32_t run)
{
    char path[256];
    snprintf(path, sizeof path, "build/fuzz/crashes/disk-%u", run);
    if (save_delta(path, img, n) == 0) {
        printf("DISK FUZZ FOUND SOMETHING: saved %s\n", path);
        printf("  reproduce with: build/fuzz/diskattacks --replay %s\n", path);
    }
}

static int fuzz(uint32_t runs, uint32_t seed)
{
    cov_shared = mmap(NULL, COV_SIZE, PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    cov_seen = calloc(COV_SIZE, 1);
    if (cov_shared == MAP_FAILED || !cov_seen) {
        fprintf(stderr, "diskfuzz: cannot map the coverage area\n");
        return 2;
    }
    rng_s += seed * 0x9E3779B97F4A7C15ull;

    /* The corpus starts as the one image that is known to work. */
    corpus[0].img = malloc(base_n);
    memcpy(corpus[0].img, base, base_n);
    corpus[0].n = base_n;
    corpus_n = 1;
    run_child(corpus[0].img, corpus[0].n);
    cov_new();

    uint8_t *img = malloc(base_n);
    int crashes = 0;
    for (uint32_t run = 1; run <= runs; run++) {
        struct entry *src = &corpus[rnd((uint32_t)corpus_n)];
        memcpy(img, src->img, src->n);
        mutate(img, src->n);

        if (run_child(img, src->n)) {
            save_crash(img, src->n, run);
            crashes++;
            if (crashes >= 8)
                break;
        }
        if (cov_new() && corpus_n < CORPUS_MAX) {
            corpus[corpus_n].img = malloc(src->n);
            memcpy(corpus[corpus_n].img, img, src->n);
            corpus[corpus_n].n = src->n;
            corpus_n++;
            char path[256];
            snprintf(path, sizeof path, "tools/fuzz/corpus/disk/cov-disk-%u", run);
            save_delta(path, img, src->n);
        }
        if (run % 2000 == 0)
            printf("  %u runs, %u edges, corpus %d\n", run, cov_count(), corpus_n);
    }
    printf("disk: %u runs, %u edges, corpus %d, %d crash(es)\n",
           runs, cov_count(), corpus_n, crashes);
    return crashes ? 1 : 0;
}

static int replay(const char *path)
{
    uint8_t *img = 0;
    uint32_t n = load_delta(path, &img);
    if (!n) {                           /* a whole image, not a delta */
        load_base(path);                /* replaces the base we loaded above */
        img = base;
        n = base_n;
    }
    printf("replaying %s (%u bytes)\n", path, n);
    disk_exercise(img, n);              /* in-process: let the report through */
    printf("DISK REPLAY OK (no crash)\n");
    return 0;
}

int main(int argc, char **argv)
{
    /* A delta is relative to the base image, so that has to be in hand before
       one can be read back. argv[3] overrides it the same way --fuzz does. */
    if (argc > 2 && strcmp(argv[1], "--replay") == 0) {
        load_base(argc > 3 ? argv[3] : "build/fuzz/base.ext2");
        return replay(argv[2]);
    }

    int do_fuzz = argc > 2 && strcmp(argv[1], "--fuzz") == 0;
    const char *path = do_fuzz ? (argc > 4 ? argv[4] : "build/fuzz/base.ext2")
                               : (argc > 1 ? argv[1] : "build/fuzz/base.ext2");
    load_base(path);
    work = malloc(base_n);
    if (!work) {
        fprintf(stderr, "diskattacks: out of memory\n");
        return 2;
    }
    learn_layout();

    if (do_fuzz)
        return fuzz((uint32_t)strtoul(argv[2], NULL, 10),
                    argc > 3 ? (uint32_t)strtoul(argv[3], NULL, 10) : 1);

    test_clean();
    test_superblock();
    test_dirents();
    test_inodes();
    test_junk();

    printf("\n%d passed, %d failed\n", passed, failed);
    if (failed) {
        printf("DISKATTACKS FAILED\n");
        return 1;
    }
    printf("DISKATTACKS OK\n");
    return 0;
}
