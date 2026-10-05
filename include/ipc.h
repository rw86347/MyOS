#ifndef IPC_H
#define IPC_H

/*
 * ipc.h — what System V IPC's three kinds of object share (kernel/ipc.c):
 * shared memory (shm.c), semaphore sets (sem.c), message queues (msg.c).
 *
 * Each object has a key (how unrelated programs find it: shmget/semget/
 * msgget(key, ...)), an owner, a creator and permission bits like a file,
 * and an id. An id is (seq << 8) | slot: the slot makes every call but
 * *get-with-a-key O(1), and the sequence number, bumped when a slot is
 * freed, makes an old id stop working instead of naming someone else's
 * object.
 */
#include <stdint.h>
#include <user.h>

/* Linux's values */
#define IPC_PRIVATE 0
#define IPC_CREAT   01000
#define IPC_EXCL    02000
#define IPC_NOWAIT  04000
#define IPC_RMID    0
#define IPC_SET     1
#define IPC_STAT    2
#define IPC_INFO    3
#define IPC_64      0x100               /* *ctl: the 64-bit-ready struct (musl always) */

struct ipc_perm_k {
    int32_t key;                        /* IPC_PRIVATE once removed */
    uint32_t uid, gid, cuid, cgid;      /* owner, and who created it */
    uint16_t mode;                      /* permission bits (and SHM_DEST) */
    uint16_t seq;
};

/* The new object's owner is the caller; key and mode as given. */
void ipc_perm_init(struct ipc_perm_k *p, int32_t key, uint32_t mode);

/* Linux's ipcperms: may the caller read (4), write (2) or alter (2)? */
int  ipc_may(const struct ipc_perm_k *p, uint32_t want);
/* May the caller change or remove it (IPC_SET, IPC_RMID)? */
int  ipc_owner(const struct ipc_perm_k *p);
/* id <-> slot */
static inline int32_t ipc_id(const struct ipc_perm_k *p, int slot)
{
    return (int32_t)((uint32_t)p->seq << 8 | (uint32_t)slot);
}
/* *get's flags against an existing object with that key: 0 or -errno. */
int  ipc_check_existing(const struct ipc_perm_k *p, uint32_t flg);

/* struct ipc64_perm as programs see it, one definition for both CPUs: the
   mode is 4 bytes (i386's 2-byte mode + 2 bytes of padding are the same
   bytes, little-endian), and the last two words are longs. 36 bytes on
   i386, 48 on x86_64. */
struct ipc64_perm_user {
    int32_t key;
    uint32_t uid, gid, cuid, cgid, mode;
    uint16_t seq, pad2;
    user_addr_t unused1, unused2;
};
void ipc_perm_out(struct ipc64_perm_user *u, const struct ipc_perm_k *p);
/* IPC_SET: the new owner and mode bits from what the program gave. */
void ipc_perm_set(struct ipc_perm_k *p, const struct ipc64_perm_user *u);

/* The calls themselves, for ipc() (i386) and the direct entries. */
int32_t  shm_get(int32_t key, uint32_t size, uint32_t flg);
intptr_t shm_attach(int32_t id, user_addr_t addr, uint32_t flg);
int32_t  shm_dt(user_addr_t addr);
int32_t  shm_ctl(int32_t id, uint32_t cmd, uintptr_t buf);

int32_t  sem_get(int32_t key, int32_t nsems, uint32_t flg);
int32_t  sem_op(int32_t id, uintptr_t sops, uint32_t nsops, uintptr_t timeout, int ts64);
int32_t  sem_ctl(int32_t id, int32_t num, uint32_t cmd, uintptr_t arg);

int32_t  msg_get(int32_t key, uint32_t flg);
int32_t  msg_snd(int32_t id, uintptr_t msgp, uintptr_t size, uint32_t flg);
intptr_t msg_rcv(int32_t id, uintptr_t msgp, uintptr_t size, intptr_t type, uint32_t flg);
int32_t  msg_ctl(int32_t id, uint32_t cmd, uintptr_t buf);

struct process;
void sem_exit(struct process *p);       /* undo its SEM_UNDO adjustments */

#endif
