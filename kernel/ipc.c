/*
 * ipc.c — what System V IPC's three kinds share (see ipc.h): permissions,
 * and the ways in. Linux i386 has one system call, ipc() (117), that leads
 * to all twelve operations, and that is what musl uses there; x86_64 (and
 * i386 since Linux 5.1) has a direct call for each.
 */
#include <errno.h>
#include <ipc.h>
#include <proc.h>
#include <syscalls.h>
#include <uaccess.h>

void ipc_perm_init(struct ipc_perm_k *p, int32_t key, uint32_t mode)
{
    const struct cred *c = &proc_current()->cred;
    p->key = key;
    p->uid = p->cuid = c->euid;
    p->gid = p->cgid = c->egid;
    p->mode = (uint16_t)(mode & 0777);
}

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
   want: 4 read, 2 write (or alter), 1 execute. */
int ipc_may(const struct ipc_perm_k *p, uint32_t want)
{
    const struct cred *c = &proc_current()->cred;
    if (c->euid == 0)
        return 1;
    uint32_t m = p->mode;
    if (c->euid == p->uid || c->euid == p->cuid)
        m >>= 6;
    else if (in_group(c, p->gid) || in_group(c, p->cgid))
        m >>= 3;
    return (want & ~m & 7) == 0;
}

int ipc_owner(const struct ipc_perm_k *p)
{
    uint32_t euid = proc_current()->cred.euid;
    return euid == 0 || euid == p->uid || euid == p->cuid;
}

int ipc_check_existing(const struct ipc_perm_k *p, uint32_t flg)
{
    if ((flg & IPC_CREAT) && (flg & IPC_EXCL))
        return -EEXIST;
    /* What the flags ask for, in every class at once (Linux). */
    return ipc_may(p, (flg >> 6 | flg >> 3 | flg) & 7) ? 0 : -EACCES;
}

void ipc_perm_out(struct ipc64_perm_user *u, const struct ipc_perm_k *p)
{
    u->key = p->key;
    u->uid = p->uid;
    u->gid = p->gid;
    u->cuid = p->cuid;
    u->cgid = p->cgid;
    u->mode = p->mode;
    u->seq = p->seq;
    u->pad2 = 0;
    u->unused1 = u->unused2 = 0;
}

void ipc_perm_set(struct ipc_perm_k *p, const struct ipc64_perm_user *u)
{
    p->uid = u->uid;
    p->gid = u->gid;
    p->mode = (uint16_t)((p->mode & ~0777) | (u->mode & 0777));
}

/* --- ipc() ---------------------------------------------------------------- */

#define IPCOP_SEMOP      1
#define IPCOP_SEMGET     2
#define IPCOP_SEMCTL     3
#define IPCOP_SEMTIMEDOP 4
#define IPCOP_MSGSND    11
#define IPCOP_MSGRCV    12
#define IPCOP_MSGGET    13
#define IPCOP_MSGCTL    14
#define IPCOP_SHMAT     21
#define IPCOP_SHMDT     22
#define IPCOP_SHMGET    23
#define IPCOP_SHMCTL    24

/* ipc(call, first, second, third, ptr, fifth): the meaning of each argument
   depends on the call, exactly as in Linux's ipc/syscall.c. The low 16 bits
   of `call` say which; the high ones a version (msgrcv and shmat's). */
static int32_t shm_at_ipc(int32_t id, user_addr_t addr, uint32_t flg, uintptr_t result)
{
    intptr_t r = shm_attach(id, addr, flg);
    if (r < 0)
        return (int32_t)r;
    /* Linux too leaves the attach in place if the address can't be
       written back. */
    user_addr_t a = (user_addr_t)r;
    return copy_to_user((void *)result, &a, sizeof a) < 0 ? -EFAULT : 0;
}

SYSCALL_DEF(sys_ipc)
{
    uint32_t call = a1 & 0xFFFF, version = a1 >> 16;
    int32_t first = (int32_t)a2;
    switch (call) {
    case IPCOP_SEMOP:      return sem_op(first, a5, a3, 0, 0);
    case IPCOP_SEMTIMEDOP: return sem_op(first, a5, a3, a6, 0);  /* 32-bit timespec */
    case IPCOP_SEMGET:     return sem_get(first, (int32_t)a3, a4);
    case IPCOP_SEMCTL: {
        user_addr_t arg;               /* ptr points at the union semun */
        if (!a5)
            return -EINVAL;
        if (get_user_word(&arg, a5) < 0)
            return -EFAULT;
        return sem_ctl(first, (int32_t)a3, a4, arg);
    }
    case IPCOP_MSGSND:     return msg_snd(first, a5, a3, a4);
    case IPCOP_MSGRCV:
        if (version == 0) {             /* ptr -> { msgp, msgtyp } */
            user_addr_t kludge[2];
            if (!a5)
                return -EINVAL;
            if (get_user_word(&kludge[0], a5) < 0 ||
                get_user_word(&kludge[1], a5 + sizeof(user_addr_t)) < 0)
                return -EFAULT;
            return msg_rcv(first, kludge[0], a3, (intptr_t)kludge[1], a4);
        }
        return msg_rcv(first, a5, a3, (intptr_t)a6, a4);
    case IPCOP_MSGGET:     return msg_get(first, a3);
    case IPCOP_MSGCTL:     return msg_ctl(first, a3, a5);
    case IPCOP_SHMAT:      return version == 1 ? -EINVAL : shm_at_ipc(first, a5, a3, a4);
    case IPCOP_SHMDT:      return shm_dt(a5);
    case IPCOP_SHMGET:     return shm_get(first, a3, a4);
    case IPCOP_SHMCTL:     return shm_ctl(first, a3, a5);
    }
    return -ENOSYS;
}

/* --- the direct calls ----------------------------------------------------- */
/* Their *ctl structs are always the 64-bit-ready ones (IPC_64). */

SYSCALL_DEF(sys_shmget)
{
    if (a2 > 0xFFFFFFFFu)
        return -EINVAL;
    return shm_get((int32_t)a1, (uint32_t)a2, a3);
}
SYSCALL_DEF(sys_shmat)       { return shm_attach((int32_t)a1, a2, a3); }
SYSCALL_DEF(sys_shmdt)       { return shm_dt(a1); }
SYSCALL_DEF(sys_shmctl)      { return shm_ctl((int32_t)a1, a2 | IPC_64, a3); }
SYSCALL_DEF(sys_semget)      { return sem_get((int32_t)a1, (int32_t)a2, a3); }
SYSCALL_DEF(sys_semop)       { return sem_op((int32_t)a1, a2, a3, 0, 1); }
SYSCALL_DEF(sys_semtimedop)  { return sem_op((int32_t)a1, a2, a3, a4, 1); }  /* 64-bit timespec */
SYSCALL_DEF(sys_semctl)      { return sem_ctl((int32_t)a1, (int32_t)a2, a3 | IPC_64, a4); }
SYSCALL_DEF(sys_msgget)      { return msg_get((int32_t)a1, a2); }
SYSCALL_DEF(sys_msgsnd)      { return msg_snd((int32_t)a1, a2, a3, a4); }
SYSCALL_DEF(sys_msgrcv)      { return msg_rcv((int32_t)a1, a2, a3, (intptr_t)a4, a5); }
SYSCALL_DEF(sys_msgctl)      { return msg_ctl((int32_t)a1, a2 | IPC_64, a3); }
