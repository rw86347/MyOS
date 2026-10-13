/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * perm.c — who may read, write, run or search a file
 *
 * The rules are Linux's (generic_permission, may_create, may_delete in
 * fs/namei.c); include/vfs.h explains them. Every check reads the calling
 * process's credentials (cred.h) and the inode's mode, uid and gid: a few
 * compares, no lookups. The supplementary groups are a short array
 * (NGROUPS, 32), scanned only when the caller is neither the owner nor in
 * the file's own group, and the group bits would decide.
 *
 * Where they are called:
 *   - the path walk (fs/vfs.c): search on every directory passed through;
 *   - open (fs/vfs.c): read and/or write on the file;
 *   - the calls that change names and attributes (kernel/sys_fs.c);
 *   - access(), chdir(), execve() (kernel/syscall.c, kernel/usermode.c).
 */
#include <errno.h>
#include <proc.h>
#include <thread.h>
#include <vfs.h>

/* The caller's credentials; NULL for a kernel thread, which is root. */
static const struct cred *caller(void)
{
    const struct process *p = proc_current();
    return p ? &p->cred : NULL;
}

static int in_groups(const struct cred *c, uint32_t fsgid, uint32_t gid)
{
    if (fsgid == gid)
        return 1;
    for (uint32_t k = 0; k < c->ngroups && k < NGROUPS; k++)
        if (c->groups[k] == gid)
            return 1;
    return 0;
}

/* The heart of it, for a given user and group (file-access or real ids). */
static int check(const struct inode *i, int mask, const struct cred *c,
                 uint32_t uid, uint32_t gid)
{
    uint32_t mode = i->mode;
    if (uid == 0) {
        /* Root: anything, except running a file that nobody may run. */
        if ((mask & MAY_EXEC) && !S_ISDIR(mode) && !(mode & 0111))
            return -EACCES;
        return 0;
    }
    uint32_t bits;
    if (uid == i->uid)
        bits = mode >> 6;               /* the owner's set */
    else if (in_groups(c, gid, i->gid))
        bits = mode >> 3;               /* the group's */
    else
        bits = mode;                    /* everyone else's */
    return (mask & ~bits & 7) ? -EACCES : 0;
}

/* access() asks "may the user who STARTED this program?" — a setuid
   program checks a path for its caller with it. It sets the thread's
   perm_real for its lookup and check, so every directory on the way is
   searched as the real user too, as Linux does (it swaps the thread's
   credentials; ours are the process's, shared with sibling threads). */
int vfs_permission_slow(const struct inode *i, int mask)
{
    const struct cred *c = caller();
    if (!c)
        return 0;
    if (thread_current()->perm_real)
        return check(i, mask, c, c->uid, c->gid);
    return check(i, mask, c, c->fsuid, c->fsgid);
}

int vfs_may_create(const struct inode *dir)
{
    return vfs_permission(dir, MAY_WRITE | MAY_EXEC);
}

int vfs_may_delete(const struct inode *dir, const struct inode *victim)
{
    int err = vfs_permission(dir, MAY_WRITE | MAY_EXEC);
    if (err)
        return err;
    /* The sticky bit: everyone may make files in /tmp, but take away only
       their own (or, in their own sticky directory, anyone's). */
    const struct cred *c = caller();
    if ((dir->mode & S_ISVTX) && c && c->fsuid != 0 &&
        c->fsuid != victim->uid && c->fsuid != dir->uid)
        return -EPERM;
    return 0;
}

int vfs_owner_or_root(const struct inode *i)
{
    const struct cred *c = caller();
    return !c || c->fsuid == 0 || c->fsuid == i->uid;
}

int vfs_in_group(uint32_t gid)
{
    const struct cred *c = caller();
    return !c || in_groups(c, c->fsgid, gid);
}

int vfs_fs_root(void)
{
    const struct cred *c = caller();
    return !c || c->fsuid == 0;
}
