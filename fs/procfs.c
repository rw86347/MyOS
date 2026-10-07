/*
 * procfs.c — /proc, the kernel's own information shown as files
 *
 * On Linux, /proc is how programs ask the kernel about processes: `ps`
 * reads /proc/<pid>/stat, `free` reads /proc/meminfo. Nothing is stored;
 * each file is made up when it's read.
 *
 * The first user is musl's ttyname(): "which terminal is fd 0?" It reads
 * the link /proc/self/fd/0 (say "/dev/tty1"), then checks that stat() of
 * that name is the same file as fstat(0). `login` refuses to run without
 * a terminal name. So for now /proc holds just that:
 *
 *   /proc/self/fd/0 ... 63    one link per fd slot (a listing shows only
 *                             the caller's open ones)
 *   /proc/self/exe            a link to the caller's program (Firefox,
 *                             and anything that asks "where am I?")
 *   /proc/self/task/          its link count is 2 + the caller's threads:
 *                             Chromium's zygote checks it is single-
 *                             threaded (3) before it forks. Empty for now.
 *   /proc/cmdline             the kernel's command line: for now the one
 *                             built in (make CMDLINE=...), e.g. "nodesktop",
 *                             which the login service reads
 *
 * The tree is built once, at mount time, from ordinary RAM nodes. What's
 * special is what a link points to: "self" means whoever is looking, so
 * each fd link is marked I_PROC_FD and vfs.c asks the CALLER's fd table
 * when the link is used. /proc/self/exe is I_PROC_EXE and asks the
 * caller's process. Looking one up is O(1) — no search, nothing
 * allocated — and there is nothing to keep up to date as processes come
 * and go. (Linux makes self a link to /proc/<pid>; that comes with the
 * per-process directories, ROADMAP 8j.)
 */
#include <errno.h>
#include <ipv4.h>
#include <proc.h>
#include <string.h>
#include <vfs.h>

#ifndef MYOS_CMDLINE
#define MYOS_CMDLINE ""
#endif
static const char cmdline_text[] = MYOS_CMDLINE "\n";

/* The links' names, "0".."63": nodes point at their name, not a copy. */
static char fd_names[FD_MAX][3];

/*
 * /proc/net/dns — the nameserver, in resolv.conf's own format, so bringing
 * the machine up is `cp /proc/net/dns /etc/resolv.conf` and no script has to
 * know how to format it.
 *
 * It exists because the address is no longer a constant. The kernel is the
 * DHCP client (net/dhcp.c), so it is the only thing that knows which
 * nameserver this network has, and musl reads /etc/resolv.conf as an ordinary
 * file. A hard-coded resolv.conf naming QEMU's 10.0.2.3 is right in QEMU and
 * wrong everywhere else, which is exactly the machine DHCP was added for.
 *
 * Filled in once, at mount time, because /proc is mounted by init and DHCP
 * finished before init started. A lease that could change later would have to
 * rebuild this -- see the renewal gap in docs/gaps.md.
 */
static char dns_text[32];

/* "nameserver 1.2.3.4\n". Written by hand: the kernel has no snprintf, and
   this wants a length back rather than a console. */
static uint32_t fmt_dns(uint32_t ip)
{
    static const char label[] = "nameserver ";
    uint32_t n = sizeof label - 1;
    memcpy(dns_text, label, n);
    for (int shift = 24; shift >= 0; shift -= 8) {
        uint32_t b = (ip >> shift) & 0xFF;
        if (b >= 100)
            dns_text[n++] = '0' + b / 100;
        if (b >= 10)
            dns_text[n++] = '0' + (b / 10) % 10;
        dns_text[n++] = '0' + b % 10;
        if (shift)
            dns_text[n++] = '.';
    }
    dns_text[n++] = '\n';
    return n;                           /* at most 11 + 15 + 1 = 27 */
}

int procfs_mount(const char *path)
{
    int err = vfs_mount_ram(path, 0555);
    if (err)
        return err;
    struct inode *proc, *self, *fd;
    err = vfs_lookup(path, &proc);      /* the new RAM directory on top */
    if (err)
        return err;
    self = vfs_add(proc, "self", 4, S_IFDIR | 0555);
    fd = self ? vfs_add(self, "fd", 2, S_IFDIR | 0555) : NULL;
    struct inode *exe = self ? vfs_add(self, "exe", 3, S_IFLNK | 0777) : NULL;
    struct inode *task = self ? vfs_add(self, "task", 4, S_IFDIR | 0555) : NULL;
    if (!fd || !exe || !task)
        return -ENOMEM;
    task->flags |= I_PROC_TASK;
    exe->flags |= I_PROC_EXE;
    exe->size = 64;                     /* Linux reports a path-sized link */
    for (int n = 0; n < FD_MAX; n++) {
        char *s = fd_names[n];
        uint32_t len = n < 10 ? 1 : 2;
        s[0] = n < 10 ? '0' + n : '0' + n / 10;
        s[1] = '0' + n % 10;
        struct inode *l = vfs_add(fd, s, len, S_IFLNK | 0700);
        if (!l)
            return -ENOMEM;
        l->flags |= I_PROC_FD;
        l->fs_ino = n;
        l->size = 64;                   /* Linux shows that for these too */
    }
    /* The node's data points straight at dns_text rather than owning a copy of
       it -- the same zero-copy trick initramfs files use. */
    struct inode *net = vfs_add(proc, "net", 3, S_IFDIR | 0555);
    struct inode *dns = net ? vfs_add(net, "dns", 3, S_IFREG | 0444) : NULL;
    if (!dns)
        return -ENOMEM;
    dns->size = fmt_dns(ipv4_dns);
    dns->data = (const uint8_t *)dns_text;
    struct inode *cmdline = vfs_add(proc, "cmdline", 7, S_IFREG | 0444);
    if (!cmdline)
        return -ENOMEM;
    cmdline->size = sizeof cmdline_text - 1;
    cmdline->data = (const uint8_t *)cmdline_text;
    return 0;
}

uint32_t procfs_nlink(const struct inode *i)
{
    if (i->flags & I_PROC_TASK) {
        struct process *p = proc_current();
        return 2 + (p ? (uint32_t)p->nthreads : 1);    /* ".", "..", a thread each */
    }
    return i->nlink;
}

/* The path of the file the caller's fd is open on, or of its program. */
int procfs_readlink(struct inode *link, char *buf, uint32_t size)
{
    if (link->flags & I_PROC_EXE) {
        struct process *p = proc_current();
        if (!p || !p->exe)
            return -ENOENT;
        return vfs_path(p->exe, buf, size);
    }
    struct file *f = fd_get(link->fs_ino);
    if (!f || !f->inode)
        return -ENOENT;
    return vfs_path(f->inode, buf, size);
}
