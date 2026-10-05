/*
 * msg.c — System V message queues: msgget, msgsnd, msgrcv, msgctl
 *
 * A queue holds typed messages (struct msgbuf: a long `mtype` > 0, then the
 * bytes). msgrcv can take the first message, the first of one type, or the
 * first whose type is at most n — a simple priority queue. A full queue
 * makes msgsnd wait, an empty one msgrcv (unless IPC_NOWAIT).
 *
 * Each message is one kmalloc block. A message's bytes are copied in BEFORE
 * the queue is looked at, and a receiver unlinks a message before copying
 * it out: the copies can sleep, and nothing here holds a queue half-changed
 * across a sleep.
 */
#include <errno.h>
#include <ipc.h>
#include <kheap.h>
#include <proc.h>
#include <signal.h>
#include <string.h>
#include <syscalls.h>
#include <thread.h>
#include <uaccess.h>
#include <vfs.h>

#define MSG_QUEUES  32                  /* queues at once (Linux's MSGMNI was 16) */
#define MSGMAX      8192                /* the biggest message */
#define MSGMNB      16384               /* bytes a queue holds, unless root raises it */
#define MSG_NOERROR 010000              /* msgrcv: cut a long message instead of E2BIG */
#define MSG_EXCEPT  020000              /* msgrcv: the first NOT of this type */

struct msg {
    struct msg *next;                   /* first: a link IS the previous message */
    int64_t type;
    uint32_t len;
    uint8_t text[];
};

struct msg_queue {
    struct ipc_perm_k perm;
    uint8_t used;                       /* 0: a free slot */
    uint32_t stime, rtime, ctime;       /* last send, receive, change */
    uint32_t cbytes, qnum, qbytes;      /* bytes in it, messages, the limit */
    int32_t lspid, lrpid;               /* last sender, last receiver */
    struct msg *head, *tail;
    struct wait_queue wait;             /* senders and receivers waiting */
};

static struct msg_queue queues[MSG_QUEUES];

/* struct msqid64_ds as programs see it: i386 splits each time into two
   words (asm-generic/msgbuf.h); x86_64 has longs. 88 / 120 bytes. */
#ifdef ARCH_X86_64
struct msqid_ds_user {
    struct ipc64_perm_user perm;
    int64_t stime, rtime, ctime;
    uint64_t cbytes, qnum, qbytes;
    int32_t lspid, lrpid;
    uint64_t unused4, unused5;
};
_Static_assert(sizeof(struct msqid_ds_user) == 120, "Linux x86_64 msqid64_ds");
#else
struct msqid_ds_user {
    struct ipc64_perm_user perm;
    uint32_t stime, stime_high, rtime, rtime_high, ctime, ctime_high;
    uint32_t cbytes, qnum, qbytes;
    int32_t lspid, lrpid;
    uint32_t unused4, unused5;
};
_Static_assert(sizeof(struct msqid_ds_user) == 88, "Linux i386 msqid64_ds");
#endif

static struct msg_queue *lookup(int32_t id)
{
    if (id < 0)
        return NULL;
    struct msg_queue *q = &queues[id & (MSG_QUEUES - 1)];
    return q->used && q->perm.seq == (uint32_t)id >> 8 ? q : NULL;
}

static int32_t id_of(const struct msg_queue *q)
{
    return ipc_id(&q->perm, (int)(q - queues));
}

int32_t msg_get(int32_t key, uint32_t flg)
{
    if (key != IPC_PRIVATE) {
        for (int k = 0; k < MSG_QUEUES; k++) {
            struct msg_queue *q = &queues[k];
            if (!q->used || q->perm.key != key)
                continue;
            int err = ipc_check_existing(&q->perm, flg);
            return err ? err : id_of(q);
        }
        if (!(flg & IPC_CREAT))
            return -ENOENT;
    }
    struct msg_queue *q = NULL;
    for (int k = 0; k < MSG_QUEUES && !q; k++)
        if (!queues[k].used)
            q = &queues[k];
    if (!q)
        return -ENOSPC;
    ipc_perm_init(&q->perm, key, flg);
    q->used = 1;
    q->stime = q->rtime = 0;
    q->ctime = time_now();
    q->cbytes = q->qnum = 0;
    q->qbytes = MSGMNB;
    q->lspid = q->lrpid = 0;
    q->head = q->tail = NULL;
    return id_of(q);
}

/* msgsnd(id, msgp, size, flags): msgp is { long mtype; char mtext[size]; } */
int32_t msg_snd(int32_t id, uintptr_t msgp, uintptr_t size, uint32_t flg)
{
    if (size > MSGMAX)
        return -EINVAL;
    user_addr_t type;                   /* a long: the program's word size */
    if (get_user_word(&type, msgp) < 0)
        return -EFAULT;
    if ((intptr_t)type <= 0)
        return -EINVAL;
    struct msg *m = kmalloc(sizeof *m + (size ? size : 1));
    if (!m)
        return -ENOMEM;
    m->next = NULL;
    m->type = (intptr_t)type;
    m->len = (uint32_t)size;
    if (copy_from_user(m->text, (void *)(msgp + sizeof type), size) < 0) {
        kfree(m);
        return -EFAULT;
    }

    int slept = 0;
    for (;;) {
        struct msg_queue *q = lookup(id);
        int32_t err = 0;
        if (!q)
            err = slept ? -EIDRM : -EINVAL;
        else if (!ipc_may(&q->perm, 2))
            err = -EACCES;
        else if (q->cbytes + m->len <= q->qbytes && q->qnum < q->qbytes) {
            if (q->tail)                /* room: in it goes */
                q->tail->next = m;
            else
                q->head = m;
            q->tail = m;
            q->cbytes += m->len;
            q->qnum++;
            q->stime = time_now();
            q->lspid = proc_current()->pid;
            wq_wake_all(&q->wait);
            return 0;
        } else if (flg & IPC_NOWAIT) {
            err = -EAGAIN;
        }
        if (err) {
            kfree(m);
            return err;
        }
        wq_sleep(&q->wait);             /* full: wait for a receiver */
        slept = 1;
        if (signal_pending()) {
            kfree(m);
            return -EINTR;
        }
    }
}

/* The message msgrcv's type and flags pick, and the link pointing at it. */
static struct msg **find(struct msg_queue *q, intptr_t type, uint32_t flg)
{
    struct msg **best = NULL;
    for (struct msg **mp = &q->head; *mp; mp = &(*mp)->next) {
        struct msg *m = *mp;
        if (type == 0)
            return mp;                  /* the first */
        if (type > 0) {
            if ((m->type == type) != ((flg & MSG_EXCEPT) != 0))
                return mp;              /* the first of (or not of) that type */
        } else if (m->type <= -type && (!best || m->type < (*best)->type)) {
            best = mp;                  /* the lowest type up to -type */
        }
    }
    return best;
}

/* msgrcv(id, msgp, size, type, flags): returns the bytes put in mtext. */
intptr_t msg_rcv(int32_t id, uintptr_t msgp, uintptr_t size, intptr_t type, uint32_t flg)
{
    if ((intptr_t)size < 0)
        return -EINVAL;
    struct msg *m = NULL;
    int slept = 0;
    for (;;) {
        struct msg_queue *q = lookup(id);
        if (!q)
            return slept ? -EIDRM : -EINVAL;
        if (!ipc_may(&q->perm, 4))
            return -EACCES;
        struct msg **mp = find(q, type, flg);
        if (mp) {
            m = *mp;
            if (m->len > size && !(flg & MSG_NOERROR))
                return -E2BIG;          /* it stays for a bigger buffer */
            *mp = m->next;              /* ours now */
            if (q->tail == m)           /* the link was the previous one's `next` */
                q->tail = mp == &q->head ? NULL : (struct msg *)mp;
            q->cbytes -= m->len;
            q->qnum--;
            q->rtime = time_now();
            q->lrpid = proc_current()->pid;
            wq_wake_all(&q->wait);      /* room for a waiting sender */
            break;
        }
        if (flg & IPC_NOWAIT)
            return -ENOMSG;
        wq_sleep(&q->wait);
        slept = 1;
        if (signal_pending())
            return -EINTR;
    }
    uint32_t n = m->len < size ? m->len : (uint32_t)size;
    user_addr_t t = (user_addr_t)m->type;
    int err = copy_to_user((void *)msgp, &t, sizeof t);
    if (!err)
        err = copy_to_user((void *)(msgp + sizeof t), m->text, n);
    kfree(m);                           /* (lost if the buffer was bad, as on Linux) */
    return err < 0 ? err : (intptr_t)n;
}

static void msg_rmid(struct msg_queue *q)
{
    struct msg *m = q->head;
    while (m) {
        struct msg *next = m->next;
        kfree(m);
        m = next;
    }
    q->head = q->tail = NULL;
    q->used = 0;
    q->perm.seq++;
    wq_wake_all(&q->wait);              /* its sleepers get EIDRM */
}

int32_t msg_ctl(int32_t id, uint32_t cmd, uintptr_t buf)
{
    int v64 = cmd & IPC_64;
    cmd &= ~IPC_64;
    if ((cmd == IPC_STAT || cmd == IPC_SET) && !v64)
        return -EINVAL;                 /* the pre-2.4 layout: nobody uses it */
    struct msqid_ds_user ds;
    if (cmd == IPC_SET && copy_from_user(&ds, (const void *)buf, sizeof ds) < 0)
        return -EFAULT;
    struct msg_queue *q = lookup(id);
    if (!q)
        return -EINVAL;
    switch (cmd) {
    case IPC_STAT:
        if (!ipc_may(&q->perm, 4))
            return -EACCES;
        memset(&ds, 0, sizeof ds);
        ipc_perm_out(&ds.perm, &q->perm);
        ds.stime = q->stime;
        ds.rtime = q->rtime;
        ds.ctime = q->ctime;
        ds.cbytes = q->cbytes;
        ds.qnum = q->qnum;
        ds.qbytes = q->qbytes;
        ds.lspid = q->lspid;
        ds.lrpid = q->lrpid;
        return copy_to_user((void *)buf, &ds, sizeof ds) < 0 ? -EFAULT : 0;
    case IPC_SET:
        if (!ipc_owner(&q->perm))
            return -EPERM;
        if (ds.qbytes > MSGMNB && proc_current()->cred.euid != 0)
            return -EPERM;              /* only root raises the limit */
        ipc_perm_set(&q->perm, &ds.perm);
        q->qbytes = (uint32_t)ds.qbytes;
        q->ctime = time_now();
        wq_wake_all(&q->wait);
        return 0;
    case IPC_RMID:
        if (!ipc_owner(&q->perm))
            return -EPERM;
        msg_rmid(q);
        return 0;
    }
    return -EINVAL;
}
