/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * shmtest — System V shared memory through musl (shmget, shmat, shmdt,
 * shmctl, all over ipc()), the way the X server's MIT-SHM and its clients
 * use it: make a segment, attach it in two processes, write in one and
 * read in the other, remove it while attached.
 *
 * Prints "shmtest: <check> ok" for each check and exits 0 if all pass.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <grp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/sysinfo.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;

static void check(int ok, const char *what)
{
    printf("shmtest: %s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok)
        failures++;
}

static int fails(long ret, int err)
{
    return ret == -1 && errno == err;
}

static int attach_failed(void *p, int err)
{
    return p == (void *)-1 && errno == err;
}

/* Run fn in a child; its exit status (wait's), or -1. */
static int in_child(void (*fn)(void *), void *arg)
{
    pid_t pid = fork();
    if (pid == 0) {
        fn(arg);
        _exit(0);
    }
    int status;
    return pid > 0 && waitpid(pid, &status, 0) == pid ? status : -1;
}

static int killed_by_segv(int status)
{
    return status != -1 && WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV;
}

static int nattch(int id)
{
    struct shmid_ds ds;
    return shmctl(id, IPC_STAT, &ds) == 0 ? (int)ds.shm_nattch : -1;
}

/* --- 1. two attaches, and fork, see the same bytes ------------------------- */

static void child_writes(void *p)
{
    strcpy(p, "written by the child");
}

static void shared(void)
{
    int id = shmget(IPC_PRIVATE, 10000, IPC_CREAT | 0600);
    char *a = shmat(id, NULL, 0), *b = shmat(id, NULL, 0);
    check(id >= 0 && a != (void *)-1 && b != (void *)-1 && a != b,
          "a private segment attaches twice, at two addresses");
    if (a == (void *)-1 || b == (void *)-1)
        return;
    a[9999] = 'z';
    strcpy(a, "hello");
    check(strcmp(b, "hello") == 0 && b[9999] == 'z', "a write through one attach shows in the other");

    struct shmid_ds ds;
    check(shmctl(id, IPC_STAT, &ds) == 0 && ds.shm_segsz == 10000 &&
          (ds.shm_perm.mode & 0777) == 0600 && ds.shm_perm.uid == geteuid() &&
          ds.shm_perm.cuid == geteuid() && ds.shm_cpid == getpid() &&
          ds.shm_nattch == 2 && ds.shm_atime != 0,
          "IPC_STAT: size, mode, owner, creator, 2 attaches, attach time");

    int st = in_child(child_writes, a);
    check(st == 0 && strcmp(a, "written by the child") == 0 && strcmp(b, a) == 0,
          "fork shares it: the parent sees the child's write (no copy-on-write)");
    check(nattch(id) == 2, "the child's exit dropped its attaches");

    check(shmdt(a) == 0 && nattch(id) == 1 && strcmp(b, "written by the child") == 0,
          "shmdt drops one attach, the other keeps the bytes");
    check(fails(shmdt(a), EINVAL) && fails(shmdt(b + 1), EINVAL),
          "shmdt of a detached or unaligned address is EINVAL");
    check(shmdt(b) == 0 && nattch(id) == 0, "the last detach leaves 0 attaches");
    check(shmctl(id, IPC_RMID, NULL) == 0 && fails(shmctl(id, IPC_STAT, &ds), EINVAL),
          "removing an unattached segment frees it at once");
}

/* --- 2. keys ---------------------------------------------------------------- */

static void keys(void)
{
    key_t key = 0x4d794f53;             /* "MyOS" */
    int id = shmget(key, 8192, IPC_CREAT | IPC_EXCL | 0644);
    check(id >= 0, "shmget with a key makes a segment");
    check(fails(shmget(key, 8192, IPC_CREAT | IPC_EXCL | 0644), EEXIST),
          "IPC_CREAT|IPC_EXCL on an existing key is EEXIST");
    check(shmget(key, 100, 0) == id && shmget(key, 0, IPC_CREAT) == id,
          "the same key finds the same segment");
    check(fails(shmget(key, 8193, 0), EINVAL), "asking for more than it has is EINVAL");
    check(fails(shmget(key + 1, 4096, 0), ENOENT), "a missing key without IPC_CREAT is ENOENT");
    check(fails(shmget(IPC_PRIVATE, 0, IPC_CREAT), EINVAL), "a new segment of size 0 is EINVAL");

    char *p = shmat(id, NULL, 0);
    check(p != (void *)-1 && shmctl(id, IPC_RMID, NULL) == 0,
          "IPC_RMID while attached succeeds");
    struct shmid_ds ds;
    check(shmctl(id, IPC_STAT, &ds) == 0 && (ds.shm_perm.mode & 01000) &&
          ds.shm_perm.__ipc_perm_key == IPC_PRIVATE,
          "a removed segment says SHM_DEST, and its key is gone");
    check(fails(shmget(key, 100, 0), ENOENT), "shmget can't find it by key any more");
    if (p != (void *)-1) {
        strcpy(p, "still here");
        char *q = shmat(id, NULL, SHM_RDONLY);
        check(q != (void *)-1 && strcmp(q, "still here") == 0,
              "it can still be attached by id until the last detach (as Linux)");
        check(shmdt(p) == 0 && nattch(id) == 1, "one detach: one attach left");
        check(q != (void *)-1 && shmdt(q) == 0 && fails(shmctl(id, IPC_STAT, &ds), EINVAL),
              "the last detach frees a removed segment");
    }
    check(shmget(key, 4096, IPC_CREAT | IPC_EXCL | 0600) >= 0,
          "the key can be used again afterwards");
    shmctl(shmget(key, 0, 0), IPC_RMID, NULL);
}

/* --- 3. read-only attaches, addresses -------------------------------------- */

static void child_writes_one(void *p)
{
    *(volatile char *)p = 1;
}

static void attaches(void)
{
    int id = shmget(IPC_PRIVATE, 3 * 4096, IPC_CREAT | 0600);
    char *rw = shmat(id, NULL, 0), *ro = shmat(id, NULL, SHM_RDONLY);
    if (rw == (void *)-1 || ro == (void *)-1) {
        check(0, "attach read-write and read-only");
        return;
    }
    rw[5000] = 'x';
    check(ro[5000] == 'x', "a read-only attach reads the segment");
    check(killed_by_segv(in_child(child_writes_one, ro)), "writing a read-only attach is SIGSEGV");
    check(fails(mprotect(ro, 4096, PROT_READ | PROT_WRITE), EACCES),
          "mprotect can't make a read-only attach writable (EACCES)");

    /* mprotect splits the read-write one into areas; shmdt removes all. */
    check(mprotect(rw + 4096, 4096, PROT_READ) == 0 &&
          killed_by_segv(in_child(child_writes_one, rw + 4096)) &&
          in_child(child_writes_one, rw + 8192) == 0,
          "mprotect on part of an attach");
    check(shmdt(rw) == 0 && killed_by_segv(in_child(child_writes_one, rw + 8192)) && nattch(id) == 1,
          "shmdt removes all of an attach mprotect split, and it is gone");

    /* A chosen address: rounded with SHM_RND, refused over another mapping
       unless SHM_REMAP. */
    char *at = mmap(NULL, 4 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    munmap(at, 4 * 4096);
    check(attach_failed(shmat(id, at + 100, 0), EINVAL), "an unaligned address is EINVAL");
    char *r = shmat(id, at + 100, SHM_RND);
    check(r == at && r[5000] == 'x', "SHM_RND rounds the address down to a page");
    check(attach_failed(shmat(id, at + 4096, 0), EINVAL), "attaching over a mapping is EINVAL");
    char *m = mmap(at + 3 * 4096, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    char *over = shmat(id, at + 2 * 4096, SHM_REMAP);
    check(m == at + 3 * 4096 && over == at + 2 * 4096 && over[5000] == 'x',
          "SHM_REMAP replaces what was there");
    check(attach_failed(shmat(id, NULL, SHM_REMAP), EINVAL), "SHM_REMAP with no address is EINVAL");
    check(attach_failed(shmat(id + 256 * 1000, NULL, 0), EINVAL) && attach_failed(shmat(-1, NULL, 0), EINVAL),
          "an id that names no segment is EINVAL");
    shmdt(ro);
    shmdt(r);
    shmdt(over);
    shmctl(id, IPC_RMID, NULL);
}

/* --- 4. permissions --------------------------------------------------------- */

static int perm_id;

static void as_other_user(void *arg)
{
    (void)arg;
    struct shmid_ds ds;
    /* Out of group 0 too: 0640 lets the segment's group read. */
    if (setgroups(0, NULL) != 0 || setgid(1000) != 0 || setuid(1000) != 0)
        _exit(1);
    if (!attach_failed(shmat(perm_id, NULL, 0), EACCES))
        _exit(2);                       /* 0640: others may not read */
    if (!fails(shmctl(perm_id, IPC_STAT, &ds), EACCES))
        _exit(3);
    if (!fails(shmctl(perm_id, IPC_RMID, NULL), EPERM))
        _exit(4);
    if (!fails(shmget(0x4d794f54, 0, 0400), EACCES))
        _exit(5);
    _exit(0);
}

static void as_owner(void *arg)
{
    (void)arg;
    char *p;
    if (setuid(1000) != 0)
        _exit(1);
    if ((p = shmat(perm_id, NULL, 0)) == (void *)-1 || strcmp(p, "for 1000") != 0)
        _exit(2);
    if (shmdt(p) != 0)
        _exit(3);
    _exit(0);
}

static void permissions(void)
{
    perm_id = shmget(0x4d794f54, 4096, IPC_CREAT | 0640);
    char *p = shmat(perm_id, NULL, 0);
    if (perm_id < 0 || p == (void *)-1) {
        check(0, "a root segment for the permission checks");
        return;
    }
    strcpy(p, "for 1000");
    int st = in_child(as_other_user, NULL);
    check(st == 0, "another user: shmat/IPC_STAT/shmget EACCES, IPC_RMID EPERM");
    if (st)
        printf("shmtest: (the other user's child: status %#x)\n", st);
    struct shmid_ds ds;
    check(shmctl(perm_id, IPC_STAT, &ds) == 0, "IPC_STAT for IPC_SET");
    ds.shm_perm.uid = 1000;
    ds.shm_perm.mode = 0600;
    check(shmctl(perm_id, IPC_SET, &ds) == 0 && shmctl(perm_id, IPC_STAT, &ds) == 0 &&
          ds.shm_perm.uid == 1000 && (ds.shm_perm.mode & 0777) == 0600 && ds.shm_perm.cuid == 0,
          "IPC_SET gives it to user 1000 with mode 0600");
    check(in_child(as_owner, NULL) == 0, "the new owner may attach it");
    shmdt(p);
    shmctl(perm_id, IPC_RMID, NULL);
}

/* --- 5. memory comes back -------------------------------------------------- */

static unsigned long free_kib(void)
{
    struct sysinfo si;
    sysinfo(&si);
    return (unsigned long)((unsigned long long)si.freeram * si.mem_unit / 1024);
}

static void memory(void)
{
    enum { SIZE = 4 << 20 };
    unsigned long before = free_kib();
    int id = shmget(IPC_PRIVATE, SIZE, IPC_CREAT | 0600);
    char *p = shmat(id, NULL, 0);
    if (p == (void *)-1) {
        check(0, "a 4 MiB segment");
        return;
    }
    check(free_kib() + 64 > before, "attaching costs no memory until touched");
    for (int i = 0; i < SIZE; i += 4096)
        p[i] = (char)i;
    unsigned long touched = free_kib();
    check(touched + 4000 < before, "touching 4 MiB uses 4 MiB");
    shmctl(id, IPC_RMID, NULL);
    check(free_kib() < touched + 256, "removing it while attached frees nothing yet");
    shmdt(p);
    check(free_kib() + 256 > before, "the last detach gives the 4 MiB back");
}

int main(void)
{
    shared();
    keys();
    attaches();
    permissions();
    memory();
    if (failures) {
        printf("shmtest: %d FAILED\n", failures);
        return 1;
    }
    printf("shmtest: all ok\n");
    return 0;
}
