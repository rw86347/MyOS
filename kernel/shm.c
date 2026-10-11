/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
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
 * The ways in (ipc() on i386, direct calls) and the permission rules are
 * in ipc.c, shared with semaphores (sem.c) and message queues (msg.c).
 *
 * Speed: an id names its slot (the low 8 bits), so everything but shmget
 * with a key is O(1). shmget with a key looks at every slot, at most
 * SHM_SEGS; it is called once per segment, never in a loop.
 */
#include <errno.h>
#include <ipc.h>
#include <kheap.h>
#include <memlayout.h>
#include <mm.h>
#include <proc.h>
#include <string.h>
#include <syscalls.h>
#include <uaccess.h>
#include <vfs.h>

/* Linux's values (the IPC_* ones are in ipc.h) */
#define SHM_RDONLY  010000
#define SHM_RND     020000
#define SHM_REMAP   040000
#define SHM_EXEC    0100000
#define SHM_DEST    01000               /* mode bit: removed, freed at the last detach */
#define SHMLBA      PAGE_SIZE

#define SHM_SEGS    256                 /* segments at once; a slot is 48 bytes */
#define SHM_MAX     (1u << 30)          /* biggest segment (Linux: no real limit) */

/* struct shmid64_ds as Linux i386 lays it out (asm-generic/shmbuf.h). */
struct shmid64_ds {
    struct ipc64_perm_user perm;
    uint32_t segsz;
    uint32_t atime, atime_high, dtime, dtime_high, ctime, ctime_high;
    int32_t  cpid, lpid;
    uint32_t nattch, unused4, unused5;
};

#ifdef ARCH_X86_64
/* ...and as Linux x86_64 does (asm/shmbuf.h): the mode is 4 bytes in the
   perm part, and times, sizes and counts are longs. 112 bytes. */
struct shmid_ds_x86_64 {
    struct ipc64_perm_user perm;
    uint64_t segsz;
    int64_t  atime, dtime, ctime;
    int32_t  cpid, lpid;
    uint64_t nattch, unused4, unused5;
};
_Static_assert(sizeof(struct shmid_ds_x86_64) == 112, "Linux x86_64 shmid64_ds");

typedef struct shmid_ds_x86_64 shmid_user;  /* what this CPU's programs read */
#else
_Static_assert(sizeof(struct shmid64_ds) == 84, "Linux i386 shmid64_ds");
typedef struct shmid64_ds shmid_user;
#endif

struct shm_seg {
    struct inode *inode;                /* NULL: a free slot */
    struct ipc_perm_k perm;             /* key, owner, mode (+ SHM_DEST), seq */
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
    return s->inode && s->perm.seq == (uint32_t)id >> 8 ? s : NULL;
}

static int32_t id_of(const struct shm_seg *s)
{
    return ipc_id(&s->perm, (int)(s - segs));
}

/* The last user of a removed segment is gone (vfs.c frees the inode and
   its pages right after this). */
static void shm_evict(struct inode *i)
{
    struct shm_seg *s = &segs[i->fs_ino];
    s->inode = NULL;
    s->perm.seq++;
}

static const struct fs_ops shm_fs = { .evict = shm_evict };

int32_t shm_get(int32_t key, uint32_t size, uint32_t flg)
{
    if (key != IPC_PRIVATE) {
        for (int k = 0; k < SHM_SEGS; k++) {
            struct shm_seg *s = &segs[k];
            if (!s->inode || s->perm.key != key)
                continue;
            int err = ipc_check_existing(&s->perm, flg);
            if (err)
                return err;
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
    ipc_perm_init(&s->perm, key, flg);
    s->atime = s->dtime = 0;
    s->ctime = i->ctime;
    s->cpid = p->pid;
    s->lpid = 0;
    return id_of(s);
}

/* Attach segment id at addr (0: anywhere). Returns the address, or -errno. */
intptr_t shm_attach(int32_t id, user_addr_t addr, uint32_t flg)
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
    if (!ipc_may(&s->perm, want))
        return -EACCES;

    struct mm *mm = mm_current();
    uint32_t len = (s->inode->size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (addr) {
        if (addr + len > USER_ADDR_END || addr + len < addr)
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
    return (intptr_t)addr;
}

int32_t shm_dt(user_addr_t addr)
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
    s->perm.mode |= SHM_DEST;
    s->perm.key = IPC_PRIVATE;          /* shmget can't find it any more */
    i->flags |= I_UNLINKED;             /* freed when the last attach goes */
    if (i->refs == 0) {
        vfs_get(i);
        vfs_put(i);                     /* nobody has it attached: free it now */
    }
}

int32_t shm_ctl(int32_t id, uint32_t cmd, uintptr_t buf)
{
    shmid_user ds;
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
        if (!ipc_may(&s->perm, 4))
            return -EACCES;
        memset(&ds, 0, sizeof ds);
        ipc_perm_out(&ds.perm, &s->perm);
        ds.segsz = s->inode->size;
        ds.atime = s->atime;
        ds.dtime = s->dtime;
        ds.ctime = s->ctime;
        ds.cpid = s->cpid;
        ds.lpid = s->lpid;
        ds.nattch = s->inode->refs;
        return copy_to_user((void *)buf, &ds, sizeof ds) < 0 ? -EFAULT : 0;
    case IPC_SET:
        if (!ipc_owner(&s->perm))
            return -EPERM;
        ipc_perm_set(&s->perm, &ds.perm);
        s->ctime = time_now();
        return 0;
    case IPC_RMID:
        if (!ipc_owner(&s->perm))
            return -EPERM;
        shm_rmid(s);
        return 0;
    }
    return -EINVAL;
}
