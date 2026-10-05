/*
 * shm.c — System V shared memory: shmget, shmat, shmdt, shmctl
 *
 * Shared memory is RAM that several processes map at once: a write by one
 * is seen by the others straight away, with no copy and no system call.
 * The X server's MIT-SHM extension is built on it. A program drawing a
 * picture puts it in a segment the server has attached too, and only
 * tells it "draw from there", instead of sending megabytes down a socket.
 *
 *   shmget(key, size, flags)  make a segment (or find the one with `key`):
 *                             returns its id, a small number anyone may use
 *   shmat(id, addr, flags)    map it into this process: returns where
 *   shmdt(addr)               unmap it again
 *   shmctl(id, cmd, buf)      read or change its owner and permissions,
 *                             or remove it (IPC_RMID)
 *
 * A segment is an inode that's in no directory, its contents RAM pages
 * like a file's (ramfs.c), made on first touch. An attach is a memory area
 * with VM_SHARED (mm.c) that maps those very pages, so the inode's count
 * of users IS the count of attaches: fork, exit and exec keep it right
 * with no code here. Removing a segment only marks it while someone still
 * has it attached (programs remove it right after attaching, so it can't
 * outlive them); the last detach frees it through the normal "deleted
 * file, last user gone" path, which calls shm_evict below.
 *
 * Linux i386 reaches all four through one system call, ipc() (117), and
 * that is what musl uses. Semaphores and message queues, the other two
 * kinds of System V IPC, have no user here: ipc() says ENOSYS for them.
 *
 * Speed: an id names its slot (the low 8 bits), so everything but shmget
 * with a key is O(1). shmget with a key looks at every slot, at most
 * SHM_SEGS; it is called once per segment, never in a loop.
 */
#include <errno.h>
#include <kheap.h>
#include <memlayout.h>
#include <mm.h>
#include <proc.h>
#include <string.h>
#include <syscalls.h>
#include <uaccess.h>
#include <vfs.h>

/* Linux's values */
#define IPC_PRIVATE 0
#define IPC_CREAT   01000
#define IPC_EXCL    02000
#define IPC_RMID    0
#define IPC_SET     1
#define IPC_STAT    2
#define IPC_64      0x100               /* shmctl: the 64-bit-ready struct (musl always) */
#define SHM_RDONLY  010000
#define SHM_RND     020000
#define SHM_REMAP   040000
#define SHM_EXEC    0100000
#define SHM_DEST    01000               /* mode bit: removed, freed at the last detach */
#define SHMLBA      PAGE_SIZE

/* ipc()'s first argument: which call */
#define IPC_SHMAT   21
#define IPC_SHMDT   22
#define IPC_SHMGET  23
#define IPC_SHMCTL  24

#define SHM_SEGS    256                 /* segments at once; a slot is 48 bytes */
#define SHM_MAX     (1u << 30)          /* biggest segment (Linux: no real limit) */

/* struct shmid64_ds as Linux i386 lays it out (asm-generic/shmbuf.h). */
struct ipc64_perm {
    int32_t  key;
    uint32_t uid, gid, cuid, cgid;
    uint16_t mode, pad1;
    uint16_t seq, pad2;
    uint32_t unused1, unused2;
};
struct shmid64_ds {
    struct ipc64_perm perm;
    uint32_t segsz;
    uint32_t atime, atime_high, dtime, dtime_high, ctime, ctime_high;
    int32_t  cpid, lpid;
    uint32_t nattch, unused4, unused5;
};
_Static_assert(sizeof(struct shmid64_ds) == 84, "Linux i386 shmid64_ds");

struct shm_seg {
    struct inode *inode;                /* NULL: a free slot */
    int32_t  key;                       /* IPC_PRIVATE once removed */
    uint16_t seq;                       /* bumped when the slot is freed */
    uint16_t mode;                      /* permissions, SHM_DEST */
    uint32_t uid, gid, cuid, cgid;      /* owner, and who created it */
    uint32_t atime, dtime, ctime;       /* last attach, detach, change */
    int32_t  cpid, lpid;                /* creator, last to attach or detach */
};

static struct shm_seg segs[SHM_SEGS];

/* An id is (seq << 8) | slot: an old id stops working once its slot is
   reused, instead of naming somebody else's segment. */
static struct shm_seg *lookup(int32_t id)
{
    if (id < 0)
        return NULL;
    struct shm_seg *s = &segs[id & (SHM_SEGS - 1)];
    return s->inode && s->seq == (uint32_t)id >> 8 ? s : NULL;
}

static int32_t id_of(const struct shm_seg *s)
{
    return (int32_t)((uint32_t)s->seq << 8 | (uint32_t)(s - segs));
}

/* The last user of a removed segment is gone (vfs.c frees the inode and
   its pages right after this). */
static void shm_evict(struct inode *i)
{
    struct shm_seg *s = &segs[i->fs_ino];
    s->inode = NULL;
    s->seq++;
}

static const struct fs_ops shm_fs = { .evict = shm_evict };

static int in_group(const struct cred *c, uint32_t gid)
{
    if (c->fsgid == gid)
        return 1;
    for (uint32_t k = 0; k < c->ngroups; k++)
        if (c->groups[k] == gid)
            return 1;
    return 0;
}

/* Linux's ipcperms: the owner's bits for its owner or creator, the
   group's for their groups, else the others'. Root may do anything.
   want: 4 read, 2 write, 1 execute. */
static int may(const struct shm_seg *s, uint32_t want)
{
    const struct cred *c = &proc_current()->cred;
    if (c->euid == 0)
        return 1;
    uint32_t m = s->mode;
    if (c->euid == s->uid || c->euid == s->cuid)
        m >>= 6;
    else if (in_group(c, s->gid) || in_group(c, s->cgid))
        m >>= 3;
    return (want & ~m & 7) == 0;
}

static int is_owner(const struct shm_seg *s)
{
    uint32_t euid = proc_current()->cred.euid;
    return euid == 0 || euid == s->uid || euid == s->cuid;
}

static int32_t shm_get(int32_t key, uint32_t size, uint32_t flg)
{
    if (key != IPC_PRIVATE) {
        for (int k = 0; k < SHM_SEGS; k++) {
            struct shm_seg *s = &segs[k];
            if (!s->inode || s->key != key)
                continue;
            if ((flg & IPC_CREAT) && (flg & IPC_EXCL))
                return -EEXIST;
            /* What the flags ask for, in every class at once (Linux). */
            if (!may(s, (flg >> 6 | flg >> 3 | flg) & 7))
                return -EACCES;
            if (size > s->inode->size)
                return -EINVAL;
            return id_of(s);
        }
        if (!(flg & IPC_CREAT))
            return -ENOENT;
    }
    if (size == 0 || size > SHM_MAX)
        return -EINVAL;
    struct shm_seg *s = NULL;
    for (int k = 0; k < SHM_SEGS && !s; k++)
        if (!segs[k].inode)
            s = &segs[k];
    if (!s)
        return -ENOSPC;
    struct inode *i = kmalloc(sizeof *i);
    if (!i)
        return -ENOMEM;
    memset(i, 0, sizeof *i);
    i->mode = S_IFREG | (flg & 0777);
    i->size = size;
    i->fs = &shm_fs;
    i->fs_ino = s - segs;
    i->atime = i->mtime = i->ctime = time_now();

    const struct process *p = proc_current();
    s->inode = i;
    s->key = key;
    s->mode = flg & 0777;
    s->uid = s->cuid = p->cred.euid;
    s->gid = s->cgid = p->cred.egid;
    s->atime = s->dtime = 0;
    s->ctime = i->ctime;
    s->cpid = p->pid;
    s->lpid = 0;
    return id_of(s);
}

static int32_t shm_at(int32_t id, uint32_t addr, uint32_t flg, uint32_t result)
{
    struct shm_seg *s = lookup(id);
    if (!s)
        return -EINVAL;
    if (addr & (SHMLBA - 1)) {
        if (!(flg & SHM_RND))
            return -EINVAL;
        addr &= ~(SHMLBA - 1);
    }
    uint32_t want = flg & SHM_RDONLY ? 4 : 6;
    if (flg & SHM_EXEC)
        want |= 1;
    if (!may(s, want))
        return -EACCES;

    struct mm *mm = mm_current();
    uint32_t len = (s->inode->size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (addr) {
        if (addr + len > KERNEL_VBASE || addr + len < addr)
            return -EINVAL;
        if (flg & SHM_REMAP)
            mm_remove(mm, addr, addr + len);    /* else something there is EINVAL */
    } else if ((flg & SHM_REMAP) || !(addr = mm_find_gap(mm, len))) {
        return flg & SHM_REMAP ? -EINVAL : -ENOMEM;
    }
    uint32_t vflags = VM_SHARED | VM_READ |
                      (flg & SHM_RDONLY ? 0 : VM_WRITE | VM_MAYWRITE) |
                      (flg & SHM_EXEC ? VM_EXEC : 0);
    int err = mm_add_file(mm, addr, addr + len, vflags, s->inode, 0, ~0u);
    if (err)
        return err;
    s->atime = time_now();
    s->lpid = proc_current()->pid;
    /* Linux too leaves the attach in place if the address can't be
       written back. */
    return copy_to_user((void *)result, &addr, sizeof addr) < 0 ? -EFAULT : 0;
}

static int32_t shm_dt(uint32_t addr)
{
    if (addr & (PAGE_SIZE - 1))
        return -EINVAL;
    struct inode *seg = mm_shm_detach(addr);
    if (!seg)
        return -EINVAL;
    struct shm_seg *s = &segs[seg->fs_ino];     /* still its slot: we hold it */
    s->dtime = time_now();
    s->lpid = proc_current()->pid;
    vfs_put(seg);                               /* may free it (removed, last attach) */
    return 0;
}

static void shm_rmid(struct shm_seg *s)
{
    struct inode *i = s->inode;
    s->mode |= SHM_DEST;
    s->key = IPC_PRIVATE;               /* shmget can't find it any more */
    i->flags |= I_UNLINKED;             /* freed when the last attach goes */
    if (i->refs == 0) {
        vfs_get(i);
        vfs_put(i);                     /* nobody has it attached: free it now */
    }
}

static int32_t shm_ctl(int32_t id, uint32_t cmd, uint32_t buf)
{
    struct shmid64_ds ds;
    int v64 = cmd & IPC_64;
    cmd &= ~IPC_64;
    if ((cmd == IPC_STAT || cmd == IPC_SET) && !v64)
        return -EINVAL;                 /* the pre-2.4 layout: nobody uses it */
    /* Copy in before looking the id up: the copy can sleep, and the
       segment can go meanwhile. */
    if (cmd == IPC_SET && copy_from_user(&ds, (const void *)buf, sizeof ds) < 0)
        return -EFAULT;
    struct shm_seg *s = lookup(id);
    if (!s)
        return -EINVAL;

    switch (cmd) {
    case IPC_STAT:
        if (!may(s, 4))
            return -EACCES;
        memset(&ds, 0, sizeof ds);
        ds.perm.key = s->key;
        ds.perm.uid = s->uid;
        ds.perm.gid = s->gid;
        ds.perm.cuid = s->cuid;
        ds.perm.cgid = s->cgid;
        ds.perm.mode = s->mode;
        ds.perm.seq = s->seq;
        ds.segsz = s->inode->size;
        ds.atime = s->atime;
        ds.dtime = s->dtime;
        ds.ctime = s->ctime;
        ds.cpid = s->cpid;
        ds.lpid = s->lpid;
        ds.nattch = s->inode->refs;
        return copy_to_user((void *)buf, &ds, sizeof ds) < 0 ? -EFAULT : 0;
    case IPC_SET:
        if (!is_owner(s))
            return -EPERM;
        s->uid = ds.perm.uid;
        s->gid = ds.perm.gid;
        s->mode = (s->mode & ~0777) | (ds.perm.mode & 0777);
        s->ctime = time_now();
        return 0;
    case IPC_RMID:
        if (!is_owner(s))
            return -EPERM;
        shm_rmid(s);
        return 0;
    }
    return -EINVAL;
}

/* int ipc(unsigned call, int first, unsigned long second,
           unsigned long third, void *ptr, long fifth)
   The low 16 bits of `call` pick the operation; the high 16 are a version
   (1 was for iBCS2's shmat, which returned the address differently). */
SYSCALL_DEF(sys_ipc)
{
    uint32_t call = a1 & 0xFFFF, version = a1 >> 16;
    switch (call) {
    case IPC_SHMAT:  return version == 1 ? -EINVAL : shm_at((int32_t)a2, a5, a3, a4);
    case IPC_SHMDT:  return shm_dt(a5);
    case IPC_SHMGET: return shm_get((int32_t)a2, a3, a4);
    case IPC_SHMCTL: return shm_ctl((int32_t)a2, a3, a5);
    }
    return -ENOSYS;
}
