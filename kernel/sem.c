/*
 * sem.c — System V semaphore sets: semget, semop / semtimedop, semctl
 *
 * A semaphore is a counter that never goes below zero. semop() takes a list
 * of operations on one set — add n, take n (waiting until it can), wait
 * until it is zero — and does ALL of them or none, atomically: that is
 * what lets several processes lock several things at once without
 * deadlocking each other halfway. Here "atomically" comes for free: a
 * system call runs with interrupts off and only stops where it sleeps, and
 * the check-and-apply below never sleeps.
 *
 * SEM_UNDO: the kernel remembers, per process, how much each of its
 * operations changed each semaphore, and gives it back when the process
 * exits — so a program killed while holding a lock doesn't leave it held.
 *
 * Limits are fixed and small (a slot table, like shm.c): an id names its
 * slot, so everything but semget with a key is O(1) in the number of sets.
 * semop is O(operations); its waiters all wake on any change and look again
 * (sets are small and contention is rare in the programs that use these).
 */
#include <errno.h>
#include <interrupts.h>
#include <ipc.h>
#include <kheap.h>
#include <proc.h>
#include <signal.h>
#include <string.h>
#include <syscalls.h>
#include <thread.h>
#include <timer.h>
#include <uaccess.h>
#include <vfs.h>

#define SEM_SETS   64                   /* sets at once */
#define SEM_MAX    250                  /* semaphores per set (Linux's old SEMMSL) */
#define SEM_OPS    500                  /* operations per semop (SEMOPM) */
#define SEMVMX     32767                /* the most a semaphore can hold */
#define SEM_UNDO   0x1000

#define GETPID     11
#define GETVAL     12
#define GETALL     13
#define GETNCNT    14
#define GETZCNT    15
#define SETVAL     16
#define SETALL     17

struct sem {
    uint16_t val;
    uint16_t ncnt, zcnt;                /* waiting for it to grow / to be 0 */
    int32_t pid;                        /* the last to change it */
};

struct sem_set {
    struct ipc_perm_k perm;
    uint32_t nsems;                     /* 0: a free slot */
    uint32_t otime, ctime;              /* last semop, last change */
    struct sem *sems;
    struct wait_queue wait;             /* semop sleepers */
};

static struct sem_set sets[SEM_SETS];

/* struct sembuf, the same on both CPUs: 6 bytes. */
struct sembuf_user {
    uint16_t num;
    int16_t op, flg;
};

/* struct semid64_ds, one definition for both CPUs (asm/sembuf.h): longs
   after the perm part — i386's "_high" words are x86_64's unused ones.
   64 bytes on i386, 104 on x86_64. */
struct semid_ds_user {
    struct ipc64_perm_user perm;
    user_addr_t otime, otime_high, ctime, ctime_high;
    user_addr_t nsems, unused3, unused4;
};

/* One SEM_UNDO record: what a process's operations on one semaphore have
   added up to; it is subtracted again when the process exits. */
struct sem_undo {
    struct sem_undo *next;
    int32_t id;                         /* the set's full id: a reused slot won't match */
    uint16_t num;
    int16_t adj;
};

static struct sem_set *lookup(int32_t id)
{
    if (id < 0)
        return NULL;
    struct sem_set *s = &sets[id & (SEM_SETS - 1)];
    return s->nsems && s->perm.seq == (uint32_t)id >> 8 ? s : NULL;
}

static int32_t id_of(const struct sem_set *s)
{
    return ipc_id(&s->perm, (int)(s - sets));
}

int32_t sem_get(int32_t key, int32_t nsems, uint32_t flg)
{
    if (nsems < 0 || nsems > SEM_MAX)
        return -EINVAL;
    if (key != IPC_PRIVATE) {
        for (int k = 0; k < SEM_SETS; k++) {
            struct sem_set *s = &sets[k];
            if (!s->nsems || s->perm.key != key)
                continue;
            int err = ipc_check_existing(&s->perm, flg);
            if (err)
                return err;
            if ((uint32_t)nsems > s->nsems)
                return -EINVAL;
            return id_of(s);
        }
        if (!(flg & IPC_CREAT))
            return -ENOENT;
    }
    if (nsems == 0)
        return -EINVAL;
    struct sem_set *s = NULL;
    for (int k = 0; k < SEM_SETS && !s; k++)
        if (!sets[k].nsems)
            s = &sets[k];
    if (!s)
        return -ENOSPC;
    struct sem *sems = kmalloc(nsems * sizeof *sems);
    if (!sems)
        return -ENOMEM;
    memset(sems, 0, nsems * sizeof *sems);
    ipc_perm_init(&s->perm, key, flg);
    s->sems = sems;
    s->nsems = nsems;
    s->otime = 0;
    s->ctime = time_now();
    return id_of(s);
}

/* --- SEM_UNDO ---------------------------------------------------------------- */

static struct sem_undo **undo_slot(struct process *p, int32_t id, uint16_t num)
{
    struct sem_undo **u = (struct sem_undo **)&p->sem_undo;
    while (*u && ((*u)->id != id || (*u)->num != num))
        u = &(*u)->next;
    return u;
}

/* Forget every process's adjustment of semaphore num (or the whole set
   with num = -1) of set id: SETVAL / SETALL / IPC_RMID reset them. */
static void undo_forget(int32_t id, int num)
{
    for (int k = 0; k < PROC_MAX; k++) {
        struct process *p = proc_at(k);
        if (p->state == PROC_FREE)
            continue;
        struct sem_undo **u = (struct sem_undo **)&p->sem_undo;
        while (*u) {
            if ((*u)->id == id && (num < 0 || (*u)->num == num)) {
                struct sem_undo *dead = *u;
                *u = dead->next;
                kfree(dead);
            } else {
                u = &(*u)->next;
            }
        }
    }
}

void sem_exit(struct process *p)
{
    struct sem_undo *u = p->sem_undo;
    p->sem_undo = NULL;
    while (u) {
        struct sem_set *s = lookup(u->id);
        if (s && u->num < s->nsems && u->adj) {
            int v = s->sems[u->num].val + u->adj;
            s->sems[u->num].val = (uint16_t)(v < 0 ? 0 : v > SEMVMX ? SEMVMX : v);
            s->sems[u->num].pid = p->pid;
            s->otime = time_now();
            wq_wake_all(&s->wait);
        }
        struct sem_undo *next = u->next;
        kfree(u);
        u = next;
    }
}

/* --- semop ----------------------------------------------------------------- */

/* A timeout (struct timespec, 64- or 32-bit) -> the timer_ms() to give up
   at; 0 in *deadline means "no timeout". */
static int read_timeout(uintptr_t ut, int ts64, uint32_t *deadline)
{
    int64_t sec, nsec;
    if (ts64) {
        int64_t ts[2];
        if (copy_from_user(ts, (void *)ut, sizeof ts) < 0)
            return -EFAULT;
        sec = ts[0], nsec = ts[1];
    } else {
        int32_t ts[2];
        if (copy_from_user(ts, (void *)ut, sizeof ts) < 0)
            return -EFAULT;
        sec = ts[0], nsec = ts[1];
    }
    if (sec < 0 || nsec < 0 || nsec >= 1000000000)
        return -EINVAL;
    if (sec > 0x7FFFFFFF / 1000 - 1)
        sec = 0x7FFFFFFF / 1000 - 1;
    uint32_t ms = (uint32_t)sec * 1000 + (uint32_t)((nsec + 999999) / 1000000);
    *deadline = timer_ms() + ms + 1;    /* now is partway through a ms */
    if (!*deadline)
        *deadline = 1;
    return 0;
}

/* Can all of ops be done now? -1 if so, else the index of the first that
   would have to wait. -ERANGE if one would push a value past SEMVMX.
   O(semaphores + operations): a copy of the values, the ops applied to it. */
static int would_block(const struct sem_set *s, const struct sembuf_user *ops, uint32_t n)
{
    uint16_t v[SEM_MAX];                /* the values as the ops so far leave them */
    for (uint32_t k = 0; k < s->nsems; k++)
        v[k] = s->sems[k].val;
    for (uint32_t i = 0; i < n; i++) {
        int x = v[ops[i].num];
        if (ops[i].op > 0) {
            x += ops[i].op;
            if (x > SEMVMX)
                return -ERANGE;
        } else if (ops[i].op < 0) {
            if (x < -ops[i].op)
                return (int)i;
            x += ops[i].op;
        } else if (x != 0) {
            return (int)i;
        }
        v[ops[i].num] = (uint16_t)x;
    }
    return -1;
}

int32_t sem_op(int32_t id, uintptr_t usops, uint32_t nsops, uintptr_t utimeout, int ts64)
{
    if (nsops == 0)
        return -EINVAL;
    if (nsops > SEM_OPS)
        return -E2BIG;
    uint32_t deadline = 0;
    if (utimeout) {
        int err = read_timeout(utimeout, ts64, &deadline);
        if (err)
            return err;
    }
    struct sembuf_user *ops = kmalloc(nsops * sizeof *ops);
    if (!ops)
        return -ENOMEM;
    int32_t rc = 0;
    if (copy_from_user(ops, (void *)usops, nsops * sizeof *ops) < 0) {
        rc = -EFAULT;
        goto out;
    }
    int alter = 0, undo = 0;
    for (uint32_t i = 0; i < nsops; i++) {
        alter |= ops[i].op != 0;
        undo |= (ops[i].flg & SEM_UNDO) != 0;
    }

    struct process *p = proc_current();
    int slept = 0;
    for (;;) {
        struct sem_set *s = lookup(id);
        if (!s) {
            rc = slept ? -EIDRM : -EINVAL;  /* removed while we waited */
            break;
        }
        if (!ipc_may(&s->perm, alter ? 2 : 4)) {
            rc = -EACCES;
            break;
        }
        uint32_t i;
        for (i = 0; i < nsops && ops[i].num < s->nsems; i++)
            ;
        if (i < nsops) {
            rc = -EFBIG;
            break;
        }
        /* SEM_UNDO records first: making one can fail, and after the
           check below nothing may fail. */
        if (undo) {
            for (i = 0; i < nsops; i++) {
                if (!(ops[i].flg & SEM_UNDO) || *undo_slot(p, id, ops[i].num))
                    continue;
                struct sem_undo *u = kmalloc(sizeof *u);
                if (!u) {
                    rc = -ENOMEM;
                    goto out;
                }
                *u = (struct sem_undo){ p->sem_undo, id, ops[i].num, 0 };
                p->sem_undo = u;
            }
        }
        int b = would_block(s, ops, nsops);
        if (b == -ERANGE) {
            rc = -ERANGE;
            break;
        }
        if (b < 0) {                    /* all of them can go: do them */
            for (i = 0; i < nsops; i++) {
                struct sem *m = &s->sems[ops[i].num];
                m->val = (uint16_t)(m->val + ops[i].op);
                m->pid = p->pid;
                if (ops[i].flg & SEM_UNDO)
                    (*undo_slot(p, id, ops[i].num))->adj -= ops[i].op;
            }
            s->otime = time_now();
            if (alter)
                wq_wake_all(&s->wait);  /* someone may be able to go now */
            break;
        }
        if (ops[b].flg & IPC_NOWAIT) {
            rc = -EAGAIN;
            break;
        }
        /* Wait for a change. Count ourselves as waiting on that semaphore
           (semctl GETNCNT / GETZCNT) while we do. */
        uint16_t num = ops[b].num;
        int zero = ops[b].op == 0;
        if (zero)
            s->sems[num].zcnt++;
        else
            s->sems[num].ncnt++;
        int timed_out = wq_sleep_until(&s->wait, deadline);
        slept = 1;
        if (lookup(id) == s) {
            if (zero)
                s->sems[num].zcnt--;
            else
                s->sems[num].ncnt--;
        }
        if (signal_pending()) {
            rc = -EINTR;
            break;
        }
        if (timed_out) {
            rc = -EAGAIN;
            break;
        }
    }
out:
    kfree(ops);
    return rc;
}

/* --- semctl ------------------------------------------------------------------ */

static void sem_rmid(struct sem_set *s)
{
    undo_forget(id_of(s), -1);
    kfree(s->sems);
    s->sems = NULL;
    s->nsems = 0;
    s->perm.seq++;
    wq_wake_all(&s->wait);              /* its sleepers get EIDRM */
}

int32_t sem_ctl(int32_t id, int32_t num, uint32_t cmd, uintptr_t arg)
{
    int v64 = cmd & IPC_64;
    cmd &= ~IPC_64;
    if ((cmd == IPC_STAT || cmd == IPC_SET) && !v64)
        return -EINVAL;                 /* the pre-2.4 layout: nobody uses it */
    struct semid_ds_user ds;
    uint16_t *vals = NULL;
    /* Copy in before the lookup that counts: the copy can sleep, and the
       set can go meanwhile. */
    if (cmd == IPC_SET && copy_from_user(&ds, (const void *)arg, sizeof ds) < 0)
        return -EFAULT;
    if (cmd == SETALL || cmd == GETALL) {
        vals = kmalloc(SEM_MAX * sizeof *vals);
        if (!vals)
            return -ENOMEM;
    }
    if (cmd == SETALL) {
        struct sem_set *s = lookup(id);
        if (!s || copy_from_user(vals, (const void *)arg, s->nsems * sizeof *vals) < 0) {
            kfree(vals);
            return s ? -EFAULT : -EINVAL;
        }
    }

    int32_t rc = -EINVAL;
    struct sem_set *s = lookup(id);
    if (!s)
        goto out;
    int one = cmd == GETVAL || cmd == GETPID || cmd == GETNCNT || cmd == GETZCNT ||
              cmd == SETVAL;
    if (one && (num < 0 || (uint32_t)num >= s->nsems))
        goto out;
    switch (cmd) {
    case IPC_STAT:
    case GETVAL: case GETPID: case GETNCNT: case GETZCNT: case GETALL:
        if (!ipc_may(&s->perm, 4)) {
            rc = -EACCES;
            break;
        }
        if (cmd == GETVAL)  { rc = s->sems[num].val; break; }
        if (cmd == GETPID)  { rc = s->sems[num].pid; break; }
        if (cmd == GETNCNT) { rc = s->sems[num].ncnt; break; }
        if (cmd == GETZCNT) { rc = s->sems[num].zcnt; break; }
        if (cmd == GETALL) {
            uint32_t n = s->nsems;
            for (uint32_t k = 0; k < n; k++)
                vals[k] = s->sems[k].val;
            rc = copy_to_user((void *)arg, vals, n * sizeof *vals) < 0 ? -EFAULT : 0;
            break;
        }
        memset(&ds, 0, sizeof ds);
        ipc_perm_out(&ds.perm, &s->perm);
        ds.otime = s->otime;
        ds.ctime = s->ctime;
        ds.nsems = s->nsems;
        rc = copy_to_user((void *)arg, &ds, sizeof ds) < 0 ? -EFAULT : 0;
        break;
    case SETVAL: {
        int32_t v = (int32_t)arg;       /* union semun's int: the low half */
        if (!ipc_may(&s->perm, 2)) {
            rc = -EACCES;
            break;
        }
        if (v < 0 || v > SEMVMX) {
            rc = -ERANGE;
            break;
        }
        s->sems[num].val = (uint16_t)v;
        s->sems[num].pid = proc_current()->pid;
        s->ctime = time_now();
        undo_forget(id, num);
        wq_wake_all(&s->wait);
        rc = 0;
        break;
    }
    case SETALL:
        if (!ipc_may(&s->perm, 2)) {
            rc = -EACCES;
            break;
        }
        rc = 0;
        for (uint32_t k = 0; k < s->nsems; k++)
            if (vals[k] > SEMVMX)
                rc = -ERANGE;
        if (rc)
            break;
        for (uint32_t k = 0; k < s->nsems; k++) {
            s->sems[k].val = vals[k];
            s->sems[k].pid = proc_current()->pid;
        }
        s->ctime = time_now();
        undo_forget(id, -1);
        wq_wake_all(&s->wait);
        break;
    case IPC_SET:
        if (!ipc_owner(&s->perm)) {
            rc = -EPERM;
            break;
        }
        ipc_perm_set(&s->perm, &ds.perm);
        s->ctime = time_now();
        rc = 0;
        break;
    case IPC_RMID:
        if (!ipc_owner(&s->perm)) {
            rc = -EPERM;
            break;
        }
        sem_rmid(s);
        rc = 0;
        break;
    }
out:
    kfree(vals);
    return rc;
}
