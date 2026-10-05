/*
 * vfs.c — the file tree, path lookup, open files and file descriptors
 *
 * Speed (rule 1 in CLAUDE.md): looking up a path costs one hash-table probe
 * per component ("/data/numbers.txt" = 2 probes), however many files there
 * are. The table is keyed on (directory, name), like Linux's dentry cache.
 * Disk file pages sit in the page cache (pcache.c). read() of an ext2 file
 * still copies from the block cache; exec and mmap fault through the page
 * cache, so a program is not copied whole at start.
 */
#include <errno.h>
#include <kheap.h>
#include <kprintf.h>
#include <pcache.h>
#include <proc.h>
#include <string.h>
#include <uaccess.h>
#include <vfs.h>

#define HASH_SIZE  512              /* buckets (power of two) */
#define MAX_LINKS  8                /* symlinks followed in one lookup */

/* The root directory: in RAM (the initramfs) — until a disk holding the
   whole system is mounted on "/" at boot (vfs_mount). */
static struct inode ram_root = {
    .ino = 1, .mode = S_IFDIR | 0755, .nlink = 2, .name = "", .parent = &ram_root,
};
static struct inode *root = &ram_root;
static struct inode *hash[HASH_SIZE];
static uint32_t next_ino = 2;

struct inode *vfs_root(void) { return root; }

/* The calling process's "/": the real root, or what it chroot()ed to. */
static struct inode *proc_root(void)
{
    struct process *p = proc_current();
    return (p && p->root) ? p->root : root;
}


/* FNV-1a over the name, mixed with the directory's address. */
static uint32_t hash_of(const struct inode *dir, const char *name, uint32_t len)
{
    uint32_t h = 2166136261u ^ (uint32_t)(uintptr_t)dir;
    for (uint32_t i = 0; i < len; i++)
        h = (h ^ (uint8_t)name[i]) * 16777619u;
    return h & (HASH_SIZE - 1);
}

struct inode *vfs_child(struct inode *dir, const char *name, uint32_t len)
{
    if (dir->flags & I_UNLOADED)
        vfs_dir_load(dir);              /* a disk directory, first visit */
    for (struct inode *i = hash[hash_of(dir, name, len)]; i; i = i->hash_next)
        if (i->parent == dir && i->name_len == len && memcmp(i->name, name, len) == 0)
            return i;
    return NULL;
}

/* Built backwards by climbing to the root; a mounted root is named by the
   directory it covers. Nodes outside the tree get Linux's names: the
   shared console "/dev/console", pipes and sockets "pipe:[ino]". A deleted
   file's old directory may be gone, so it gets just its name, as Linux's
   "(deleted)" form. */
int vfs_path(struct inode *i, char *buf, uint32_t size)
{
    char tmp[PATH_MAX];
    uint32_t at = PATH_MAX;
    tmp[--at] = '\0';
    if (!i->parent || (i->flags & I_UNLINKED)) {
        const char *pre = "", *post = (i->flags & I_UNLINKED) ? " (deleted)" : "";
        char num[11];
        const char *name = i->name;
        uint32_t n = i->name_len;
        if (S_ISFIFO(i->mode) || (i->mode & S_IFMT) == S_IFSOCK) {
            pre = S_ISFIFO(i->mode) ? "pipe:[" : "socket:[", post = "]";
            uint32_t v = i->ino;
            name = num + sizeof num;
            do { *(char *)--name = '0' + v % 10; v /= 10; } while (v);
            n = num + sizeof num - name;
        } else if (S_ISCHR(i->mode)) {
            pre = "/dev/";
        }
        uint32_t lp = strlen(pre), lq = strlen(post);
        if (lp + n + lq + 1 > size)
            return -ENAMETOOLONG;
        memcpy(buf, pre, lp);
        memcpy(buf + lp, name, n);
        memcpy(buf + lp + n, post, lq + 1);
        return lp + n + lq;
    }
    /* Relative to the caller's "/" (a chroot); a directory outside it is
       "(unreachable)/...", as on Linux. */
    struct inode *top = proc_root();
    int outside = 0;
    if (i == top)
        tmp[--at] = '/';
    while (i != top) {
        while (i->covers)
            i = i->covers;
        if (i == root || i->parent == i) {
            outside = top != root;
            break;                      /* "/", or a root nobody can reach */
        }
        if (i->name_len + 1 > at)
            return -ENAMETOOLONG;
        at -= i->name_len;
        memcpy(tmp + at, i->name, i->name_len);
        tmp[--at] = '/';
        i = i->parent;
    }
    if (outside) {
        static const char unreach[] = "(unreachable)";
        if (at < sizeof unreach - 1)
            return -ENAMETOOLONG;
        at -= sizeof unreach - 1;
        memcpy(tmp + at, unreach, sizeof unreach - 1);
    }
    uint32_t len = PATH_MAX - at;
    if (len > size)
        return -ENAMETOOLONG;
    memcpy(buf, tmp + at, len);
    return len - 1;
}

/* The sibling list: put c right after entry `after` (NULL: at the front). */
static void sibling_insert(struct inode *dir, struct inode *after, struct inode *c)
{
    struct inode **link = after ? &after->next_sibling : &dir->children;
    c->next_sibling = *link;
    if (*link)
        (*link)->pprev_sibling = &c->next_sibling;
    *link = c;
    c->pprev_sibling = link;
}

/* ...and out again. pprev points at whatever points at us (the list head,
   or the previous entry's `next`), so no search for the predecessor. */
static void sibling_remove(struct inode *i)
{
    *i->pprev_sibling = i->next_sibling;
    if (i->next_sibling)
        i->next_sibling->pprev_sibling = i->pprev_sibling;
}

/* Into / out of the tree: the directory's list and the lookup hash. O(1). */
static void tree_insert(struct inode *dir, struct inode *i)
{
    i->parent = dir;
    sibling_insert(dir, NULL, i);
    struct inode **head = &hash[hash_of(dir, i->name, i->name_len)];
    i->hash_next = *head;
    if (*head)
        (*head)->pprev_hash = &i->hash_next;
    *head = i;
    i->pprev_hash = head;
}

static void tree_remove(struct inode *i)
{
    sibling_remove(i);
    *i->pprev_hash = i->hash_next;
    if (i->hash_next)
        i->hash_next->pprev_hash = i->pprev_hash;
}

struct inode *vfs_add(struct inode *dir, const char *name, uint32_t len, uint32_t mode)
{
    struct inode *i = kmalloc(sizeof *i);
    if (!i)
        return NULL;
    memset(i, 0, sizeof *i);
    i->ino = next_ino++;
    i->mode = mode;
    i->nlink = S_ISDIR(mode) ? 2 : 1;
    i->name = name;
    i->name_len = len;
    tree_insert(dir, i);
    if (S_ISDIR(mode))
        dir->nlink++;                   /* the new directory's ".." */
    return i;
}

/* --- changing the tree ----------------------------------------------------- */

static void inode_free(struct inode *i)
{
    pcache_invalidate(i);
    if (i->fs && i->fs->evict)
        i->fs->evict(i);                /* on the disk: its blocks, its inode */
    file_free(i);                       /* its pages (ramfs.c) */
    if (i->flags & I_DATA_OWNED)
        kfree((void *)i->data);
    if (i->flags & I_NAME_OWNED)
        kfree((void *)i->name);
    kfree(i);
}

void vfs_put(struct inode *i)
{
    if (--i->refs == 0 && (i->flags & I_UNLINKED))
        inode_free(i);                  /* deleted earlier, and now unused */
}

int vfs_dir_load(struct inode *dir)
{
    if (!(dir->flags & I_UNLOADED))
        return 0;
    return dir->fs->load_dir(dir);      /* clears I_UNLOADED */
}

/* The tree part of a new entry: a node named by a copy of name. It's on
   the same file system as its directory. */
static int new_node(struct inode *dir, const char *name, uint32_t len, uint32_t mode,
                    struct inode **out)
{
    if (dir->flags & I_UNLINKED)
        return -ENOENT;                 /* a deleted directory takes no new entries */
    if (len == 0 || vfs_is_dots(name, len) || vfs_child(dir, name, len))
        return -EEXIST;
    if (len > NAME_MAX)
        return -ENAMETOOLONG;
    char *copy = kmalloc(len);          /* the caller's buffer won't last */
    if (!copy)
        return -ENOMEM;
    memcpy(copy, name, len);
    struct inode *i = vfs_add(dir, copy, len, mode);
    if (!i) {
        kfree(copy);
        return -ENOMEM;
    }
    i->flags |= I_NAME_OWNED;
    i->atime = i->mtime = i->ctime = time_now();
    struct process *p = proc_current();   /* NULL: the kernel, root */
    if (p) {
        i->uid = p->cred.fsuid;
        i->gid = p->cred.fsgid;
    }
    if (dir->mode & S_ISGID) {          /* a shared group directory */
        i->gid = dir->gid;
        if (S_ISDIR(mode))
            i->mode |= S_ISGID;         /* and so are its subdirectories */
    }
    i->fs = dir->fs;
    i->mnt = dir->mnt;
    *out = i;
    return 0;
}

/* The tree part of removing i (if it's on a disk, its name is gone from
   there already). */
static void detach(struct inode *i)
{
    struct inode *dir = i->parent;
    tree_remove(i);
    if (S_ISDIR(i->mode))
        dir->nlink--;
    dir->mtime = dir->ctime = i->ctime = time_now();
    i->nlink = 0;
    i->flags |= I_UNLINKED;
    if (i->refs == 0)
        inode_free(i);                  /* else: when its last user is done */
}

/* Make the new node real: on the disk too, if dir is on one. If the disk
   says no (full, broken), the node goes away again. */
static int commit_node(struct inode *dir, struct inode *i, struct inode **out)
{
    if (dir->fs) {
        int err = dir->fs->create(dir, i);
        if (err) {
            i->fs = NULL;               /* never reached the disk: nothing to evict */
            detach(i);
            return err;
        }
    }
    dir->mtime = dir->ctime = i->mtime;
    notify_entry(dir, i, IN_CREATE, 0, i->name, i->name_len);
    if (out)
        *out = i;
    return 0;
}

int vfs_create(struct inode *dir, const char *name, uint32_t len, uint32_t mode,
               uint32_t rdev, struct inode **out)
{
    struct inode *i;
    int err = new_node(dir, name, len, mode, &i);
    if (err)
        return err;
    i->rdev = rdev;
    return commit_node(dir, i, out);
}

int vfs_symlink(struct inode *dir, const char *name, uint32_t len,
                const char *target, uint32_t tlen)
{
    char *copy = kmalloc(tlen ? tlen : 1);
    if (!copy)
        return -ENOMEM;
    memcpy(copy, target, tlen);
    struct inode *i;
    int err = new_node(dir, name, len, S_IFLNK | 0777, &i);
    if (err) {
        kfree(copy);
        return err;
    }
    i->data = (const uint8_t *)copy;
    i->size = tlen;
    i->flags |= I_DATA_OWNED;
    return commit_node(dir, i, NULL);
}

int vfs_unlink(struct inode *i)
{
    if (i->flags & I_MOUNTED)
        return -EBUSY;                  /* a mounted file system's root */
    if (i->fs) {
        int err = i->fs->unlink(i->parent, i);
        if (err)
            return err;
    }
    notify_entry(i->parent, i, IN_DELETE, 0, i->name, i->name_len);
    fsnotify_deleted(i);                /* its own watches end */
    detach(i);
    return 0;
}

int vfs_truncate(struct inode *i, uint32_t size)
{
    if (i->ops && !i->fs)
        return -EINVAL;                 /* a device */
    int err = i->fs ? i->fs->truncate(i, size) : file_truncate(i, size);
    if (!err)
        notify(i, IN_MODIFY);
    return err;
}

/* An inode's mode, owner or times changed (chmod, chown, utimensat). */
int vfs_setattr(struct inode *i)
{
    notify(i, IN_ATTRIB);
    return i->fs ? i->fs->setattr(i) : 0;
}

/* Directory cursors (below) have mode 0; they're not entries. */
static struct inode *skip_cursors(struct inode *i)
{
    while (i && i->mode == 0)
        i = i->next_sibling;
    return i;
}

int vfs_dir_empty(struct inode *dir)
{
    if (vfs_dir_load(dir) < 0)
        return 0;                       /* can't tell: say no */
    return skip_cursors(dir->children) == NULL;
}

int vfs_rename(struct inode *i, struct inode *new_dir, const char *name, uint32_t len,
               int noreplace)
{
    if (new_dir->flags & I_UNLINKED)
        return -ENOENT;
    if (len > NAME_MAX)
        return -ENAMETOOLONG;
    if (i->flags & I_MOUNTED)
        return -EBUSY;
    if (i->mnt != new_dir->mnt)
        return -EXDEV;                  /* another file system: `mv` copies instead */
    struct inode *old = vfs_child(new_dir, name, len);
    if (old == i)
        return 0;                       /* renamed to itself: nothing to do */
    if (old) {
        if (noreplace)
            return -EEXIST;
        if (old->flags & I_MOUNTED)
            return -EBUSY;
        if (S_ISDIR(i->mode) && !S_ISDIR(old->mode))
            return -ENOTDIR;
        if (!S_ISDIR(i->mode) && S_ISDIR(old->mode))
            return -EISDIR;
        if (S_ISDIR(old->mode) && !vfs_dir_empty(old))
            return -ENOTEMPTY;
    }
    if (S_ISDIR(i->mode)) {             /* not into itself or its own subtree */
        for (struct inode *d = new_dir; ; d = d->parent) {
            if (d == i)
                return -EINVAL;
            if (d == root)
                break;
        }
    }
    char *copy = kmalloc(len);          /* everything that can fail, first */
    if (!copy)
        return -ENOMEM;
    memcpy(copy, name, len);
    if (i->fs) {
        int err = i->fs->rename(i, new_dir, name, len, old);
        if (err) {
            kfree(copy);
            return err;
        }
    }

    if (old) {
        fsnotify_deleted(old);
        detach(old);                    /* the replaced file */
    }
    struct inode *old_dir = i->parent;
    uint32_t cookie = ((old_dir->flags | new_dir->flags) & I_WATCHED) ? fsnotify_cookie() : 0;
    notify_entry(old_dir, i, IN_MOVED_FROM, cookie, i->name, i->name_len);
    tree_remove(i);
    if (i->flags & I_NAME_OWNED)
        kfree((void *)i->name);
    i->name = copy;
    i->name_len = len;
    i->flags |= I_NAME_OWNED;
    tree_insert(new_dir, i);
    if (S_ISDIR(i->mode) && old_dir != new_dir) {
        old_dir->nlink--;               /* its ".." moved */
        new_dir->nlink++;
    }
    uint32_t now = time_now();
    old_dir->mtime = old_dir->ctime = new_dir->mtime = new_dir->ctime = i->ctime = now;
    notify_entry(new_dir, i, IN_MOVED_TO, cookie, i->name, i->name_len);
    if (i->flags & I_WATCHED)
        fsnotify(i, IN_MOVE_SELF);
    return 0;
}

/* --- mounted file systems --------------------------------------------------- */

#define MOUNT_MAX 8
static struct mount *mounts[MOUNT_MAX];
static int nmounts;

int vfs_mount_count(void) { return nmounts; }
struct mount *vfs_mount_at(int index) { return index < nmounts ? mounts[index] : NULL; }

/* The directory at path, made if missing (a mount point). */
static int mount_point(const char *path, struct inode **out)
{
    int err = vfs_lookup(path, out);
    if (err == -ENOENT) {
        struct inode *parent;
        const char *name;
        uint32_t len;
        err = vfs_lookup_parent(NULL, path, &parent, &name, &len);
        if (!err)
            err = vfs_create(parent, name, len, S_IFDIR | 0755, 0, out);
    }
    if (err)
        return err;
    if (!S_ISDIR((*out)->mode))
        return -ENOTDIR;
    if ((*out)->mounted || *out == root)
        return -EBUSY;
    return 0;
}

/* Put r on top of directory `old`. Neither can be removed or renamed now. */
static void cover(struct inode *old, struct inode *r)
{
    old->mounted = r;
    old->flags |= I_MOUNTED;
    r->covers = old;
    r->flags |= I_MOUNTED;
}

int vfs_mount(const char *path, struct mount *m, uint32_t root_ino, uint32_t mode)
{
    if (nmounts == MOUNT_MAX)
        return -EBUSY;
    int whole = path[0] == '/' && path[1] == '\0';
    struct inode *old = NULL;
    if (!whole) {
        int err = mount_point(path, &old);
        if (err)
            return err;
    }
    struct inode *r = kmalloc(sizeof *r);   /* not in any directory list */
    if (!r)
        return -ENOMEM;
    memset(r, 0, sizeof *r);
    r->ino = next_ino++;
    r->mode = S_IFDIR | (mode & 07777);
    r->nlink = 2;
    r->fs = m->ops;
    r->mnt = m;
    r->fs_ino = root_ino;
    r->flags = I_UNLOADED;
    r->refs = 1;                        /* the mount holds it */
    if (whole) {
        r->name = "";
        r->parent = r;
        r->flags |= I_MOUNTED;
        root = r;                       /* the RAM tree stays behind, unseen */
    } else {
        r->name = old->name;
        r->name_len = old->name_len;
        r->parent = old->parent;
        cover(old, r);
    }
    m->root = r;
    m->dev_minor = 2 + nmounts;         /* "device 0:2", 0:3, ... */
    mounts[nmounts++] = m;
    return 0;
}

/*
 * pivot_root(new_root, put_old): the file system mounted at new_root becomes
 * "/", and the old root is mounted on put_old, a directory inside it — how a
 * system moves from its first root (a RAM disk) onto the real one and keeps
 * the old one reachable until it lets go. Processes whose current directory
 * was the old root move to the new one, as on Linux. The caller has checked
 * it is root and not in a chroot.
 */
int vfs_pivot_root(const char *new_path, const char *old_path)
{
    struct inode *nr, *po;
    int err = vfs_lookup(new_path, &nr);
    if (!err)
        err = vfs_lookup(old_path, &po);
    if (err)
        return err;
    if (!S_ISDIR(nr->mode) || !S_ISDIR(po->mode))
        return -ENOTDIR;
    if (nr == root || !nr->covers)
        return -EINVAL;                 /* must be a mount, other than "/" */
    if (po == nr || po->mounted)
        return -EBUSY;
    for (struct inode *i = po; i != nr; ) {     /* put_old must be under new_root */
        if (i->covers) {
            i = i->covers;
            continue;
        }
        if (i == root || i->parent == i)
            return -EINVAL;
        i = i->parent;
    }

    struct inode *old = root, *d = nr->covers;
    d->mounted = NULL;                  /* new_root leaves where it was mounted */
    d->flags &= ~I_MOUNTED;
    nr->covers = NULL;
    nr->parent = nr;                    /* ...and is "/" */
    nr->name = "";
    nr->name_len = 0;
    root = nr;
    old->name = po->name;               /* the old root, on put_old */
    old->name_len = po->name_len;
    old->parent = po->parent;
    cover(po, old);
    for (int k = 0; k < PROC_MAX; k++) {
        struct process *p = proc_at(k);
        if (p->state == PROC_ALIVE && p->cwd == old)
            proc_set_cwd(p, nr);
    }
    return 0;
}

int vfs_mount_tree(const char *path, struct inode *tree)
{
    struct inode *old;
    int err = mount_point(path, &old);
    if (err)
        return err;
    cover(old, tree);
    return 0;
}

int vfs_mount_ram(const char *path, uint32_t mode)
{
    struct inode *old;
    int err = vfs_lookup(path, &old);
    if (err)
        return err;
    if (!S_ISDIR(old->mode))
        return -ENOTDIR;
    if (old->mounted || old == root)
        return -EBUSY;
    struct inode *r = kmalloc(sizeof *r);   /* a RAM directory in no list */
    if (!r)
        return -ENOMEM;
    memset(r, 0, sizeof *r);
    r->ino = next_ino++;
    r->mode = S_IFDIR | (mode & 07777);
    r->nlink = 2;
    r->refs = 1;                        /* the mount holds it */
    r->name = old->name;
    r->name_len = old->name_len;
    r->parent = old->parent;
    cover(old, r);
    return 0;
}

/* --- path lookup -------------------------------------------------------- */

/* Walk path[0..len) starting in `dir`. A symbolic link along the way is
   replaced by its target (read straight from the archive), resolved from
   the link's own directory; `links` counts how many we've followed, to
   stop loops (a -> b -> a). */
static int walk(struct inode *dir, const char *path, uint32_t len, int follow_last,
                int *links, struct inode **out)
{
    /* ".." stops at the process's "/" (no way out of a chroot). Looked up
       only when a path needs it: most are relative and have no "..". */
    struct inode *top = NULL;
    struct inode *cur = (len && path[0] == '/') ? (top = proc_root()) : dir;
    uint32_t pos = 0;

    for (;;) {
        while (pos < len && path[pos] == '/')
            pos++;
        if (pos >= len)
            break;
        const char *name = path + pos;
        uint32_t n = 0;
        while (pos + n < len && path[pos + n] != '/')
            n++;
        pos += n;
        uint32_t rest = pos;
        while (rest < len && path[rest] == '/')
            rest++;
        int last = rest >= len;             /* nothing but slashes after it */

        if (!S_ISDIR(cur->mode))
            return -ENOTDIR;
        if (n > NAME_MAX)
            return -ENAMETOOLONG;
        if (n == 1 && name[0] == '.')
            continue;
        if (cur->flags & I_UNLINKED)
            return -ENOENT;         /* a deleted directory is empty, even of ".." */
        if (n == 2 && name[0] == '.' && name[1] == '.') {
            if (!top)
                top = proc_root();
            if (cur != root && cur != top) {
                while (cur->covers)
                    cur = cur->covers;  /* out of a mounted file system */
                cur = cur->parent;
                while (cur->mounted)
                    cur = cur->mounted;
            }
            continue;
        }
        struct inode *next = vfs_child(cur, name, n);
        if (!next)
            return -ENOENT;
        while (next->mounted)
            next = next->mounted;       /* a mount point: go into what's on it */
        if (next->flags & I_PROC_FD) {
            /* Following one gives the open file's own inode, not a path
               looked up again (Linux's "magic" links): O(1), and right
               even for a deleted file. Pipes and sockets aren't in the
               tree and can't be opened again this way. */
            struct file *f = fd_get(next->fs_ino);
            if (!f || !f->inode)
                return -ENOENT;         /* the caller has no such fd */
            if (!last || follow_last) {
                struct inode *t = f->inode;
                /* Pipes and sockets aren't in the tree and can't be opened
                   again this way. An unlinked regular file can — memfd and
                   a deleted-but-open file — which is how Firefox makes a
                   read-only copy of a memfd. */
                if (S_ISFIFO(t->mode) || (t->mode & S_IFMT) == S_IFSOCK)
                    return -ENXIO;
                next = t;
            }
        } else if (next->flags & I_PROC_EXE) {
            struct process *p = proc_current();
            if (!p || !p->exe)
                return -ENOENT;
            if (!last || follow_last)
                next = p->exe;
        } else if (S_ISLNK(next->mode) && (!last || follow_last)) {
            if (++*links > MAX_LINKS)
                return -ELOOP;
            int err = walk(cur, (const char *)next->data, next->size, 1, links, &next);
            if (err)
                return err;
        }
        cur = next;
    }
    *out = cur;
    return 0;
}

/* Absolute paths start at "/", relative ones at `dir` — by default the
   current directory. */
static struct inode *start_dir(struct inode *dir)
{
    if (dir)
        return dir;
    struct process *proc = proc_current();
    return (proc && proc->cwd) ? proc->cwd : root;
}

int vfs_lookup_at(struct inode *dir, const char *path, int follow, struct inode **out)
{
    int links = 0;
    return walk(start_dir(dir), path, strlen(path), follow, &links, out);
}

int vfs_lookup_parent(struct inode *start, const char *path, struct inode **dir,
                      const char **name, uint32_t *len)
{
    uint32_t end = strlen(path);
    while (end > 1 && path[end - 1] == '/')
        end--;                          /* "dir/" names "dir" */
    uint32_t slash = end;
    while (slash > 0 && path[slash - 1] != '/')
        slash--;
    *name = path + slash;               /* "/" leaves an empty name */
    *len = end - slash;
    if (*len > NAME_MAX)
        return -ENAMETOOLONG;
    int links = 0;
    int err = walk(start_dir(start), path, slash, 1, &links, dir);
    if (!err && !S_ISDIR((*dir)->mode))
        err = -ENOTDIR;
    return err;
}

int vfs_lookup(const char *path, struct inode **out)
{
    return vfs_lookup_at(NULL, path, 1, out);
}

int vfs_lookup_nofollow(const char *path, struct inode **out)
{
    return vfs_lookup_at(NULL, path, 0, out);
}

/* --- directories ------------------------------------------------------------ */

/*
 * Reading a directory takes several getdents64 calls (musl reads 2 KiB at
 * a time), and in between, entries may be deleted — `rm -r` deletes each
 * entry right after reading it. So an open directory doesn't remember
 * "the next entry" (which could be freed by then) but keeps a CURSOR: a
 * dummy entry (mode 0) in the directory's own list, right after the last
 * entry it returned. Deleting entries around it can't disturb it. (Linux
 * does the same in its RAM file systems.) Small directories, read in one
 * call, never need one.
 */
#define DIR_END ((void *)1)             /* f->priv: read to the end */

struct inode *vfs_dir_first(struct file *f)
{
    vfs_dir_load(f->inode);             /* a disk directory: read it first */
    if (f->priv == DIR_END)
        return NULL;
    struct inode *cursor = f->priv;
    return skip_cursors(cursor ? cursor->next_sibling : f->inode->children);
}

struct inode *vfs_dir_after(struct inode *i)
{
    return skip_cursors(i->next_sibling);
}

int vfs_dir_stop(struct file *f, struct inode *last)
{
    struct inode *cursor = f->priv;
    int have = cursor && cursor != DIR_END;
    if (!last) {                        /* all done: no cursor needed */
        if (have) {
            sibling_remove(cursor);
            kfree(cursor);
        }
        f->priv = DIR_END;
        return 0;
    }
    if (have) {
        sibling_remove(cursor);
    } else {
        cursor = kmalloc(sizeof *cursor);
        if (!cursor)
            return -ENOMEM;
        memset(cursor, 0, sizeof *cursor);  /* mode 0: not a real entry */
        f->priv = cursor;
    }
    sibling_insert(f->inode, last, cursor);
    return 0;
}

void vfs_dir_rewind(struct file *f)
{
    if (f->priv && f->priv != DIR_END) {
        sibling_remove(f->priv);
        kfree(f->priv);
    }
    f->priv = NULL;
    f->pos = 0;
}

static int32_t dir_read(struct file *f, void *b, uint32_t n)
{
    (void)f; (void)b; (void)n;
    return -EISDIR;
}

static int32_t dir_write(struct file *f, const void *b, uint32_t n)
{
    (void)f; (void)b; (void)n;
    return -EISDIR;
}

static const struct file_ops dir_ops = {
    .read = dir_read, .write = dir_write, .release = vfs_dir_rewind,
};

/* --- open files ----------------------------------------------------------- */

struct file *file_new(const struct file_ops *ops, struct inode *i, uint32_t flags)
{
    struct file *f = kmalloc(sizeof *f);
    if (!f)
        return NULL;
    f->ops = ops;
    f->inode = i;
    f->pos = 0;
    f->flags = flags;
    f->refs = 1;
    f->priv = NULL;
    f->flock = 0;
    f->kbuf = 0;
    static uint32_t next_serial = 1;    /* 0 means "no file" to a holder */
    f->serial = next_serial++;
    if (i)
        vfs_get(i);                     /* an open file keeps its inode alive */
    return f;
}

int vfs_open_at(struct inode *dir, const char *path, uint32_t flags, uint32_t mode,
                struct file **out)
{
    struct inode *i;
    int err = vfs_lookup_at(dir, path, !(flags & O_NOFOLLOW), &i);
    if (err == -ENOENT && (flags & O_CREAT)) {
        struct inode *parent;
        const char *name;
        uint32_t len;
        err = vfs_lookup_parent(dir, path, &parent, &name, &len);
        if (!err)
            err = vfs_create(parent, name, len, S_IFREG | (mode & 07777), 0, &i);
        if (err)
            return err;
    } else if (err) {
        return err;
    } else if ((flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) {
        return -EEXIST;
    }
    if (S_ISLNK(i->mode))
        return -ELOOP;              /* O_NOFOLLOW on a symlink */
    if ((flags & O_DIRECTORY) && !S_ISDIR(i->mode))
        return -ENOTDIR;
    int writing = (flags & O_ACCMODE) != O_RDONLY;
    if (S_ISDIR(i->mode) && (writing || (flags & O_TRUNC)))
        return -EISDIR;
    if (S_ISFIFO(i->mode))
        return fifo_open(i, flags, out);
    if (!i->ops && !S_ISREG(i->mode) && !S_ISDIR(i->mode))
        return -ENXIO;              /* a device node with no driver */
    if ((flags & O_TRUNC) && writing && S_ISREG(i->mode)) {
        err = vfs_truncate(i, 0);
        if (err)
            return err;
    }

    const struct file_ops *ops = i->ops ? i->ops : S_ISDIR(i->mode) ? &dir_ops : &ram_ops;
    struct file *f = file_new(ops, i, flags);
    if (!f)
        return -ENOMEM;
    if (ops->open && (err = ops->open(f)) < 0) {
        vfs_put(i);
        kfree(f);
        return err;
    }
    notify(i, IN_OPEN);
    *out = f;
    return 0;
}

void file_put(struct file *f)
{
    if (--f->refs == 0) {
        lock_file_gone(f);
        notify(f->inode, (f->flags & O_ACCMODE) != O_RDONLY ? IN_CLOSE_WRITE
                                                           : IN_CLOSE_NOWRITE);
        if (f->flock)
            flock_release(f);
        if (f->ops->release)
            f->ops->release(f);     /* e.g. a pipe end closing */
        if (f != &console_file) {
            if (f->inode)
                vfs_put(f->inode);
            kfree(f);
        }
    }
}

/* --- file descriptors ----------------------------------------------------- */
/* An fd is an index into the current process's file table. */

int fd_install(struct file *f)
{
    struct file **files = proc_current()->files;
    for (int fd = 0; fd < FD_MAX; fd++) {       /* lowest free fd, like POSIX */
        if (!files[fd]) {
            files[fd] = f;
            return fd;
        }
    }
    return -EMFILE;
}

int fd_install_at(int fd, struct file *f)
{
    if (fd < 0 || fd >= FD_MAX)
        return -EBADF;
    struct process *p = proc_current();
    struct file *old = p->files[fd];
    p->files[fd] = f;
    p->cloexec &= ~(1ull << fd);
    if (old) {
        lock_close(p, old->inode);
        file_put(old);              /* dup2 closes what was there */
    }
    return fd;
}

struct file *fd_get(int fd)
{
    struct process *p = proc_current();
    if (fd < 0 || fd >= FD_MAX || !p)
        return NULL;
    return p->files[fd];
}

int fd_close(int fd)
{
    struct file *f = fd_get(fd);
    if (!f)
        return -EBADF;
    struct process *p = proc_current();
    p->files[fd] = NULL;
    p->cloexec &= ~(1ull << fd);
    lock_close(p, f->inode);            /* POSIX: any close drops its record locks */
    file_put(f);
    return 0;
}

void fd_open_console(struct process *p)
{
    for (int fd = 0; fd < 3; fd++) {
        p->files[fd] = &console_file;
        console_file.refs++;
    }
}

void fd_copy_all(struct process *dst, struct process *src)
{
    for (int fd = 0; fd < FD_MAX; fd++) {
        dst->files[fd] = src->files[fd];
        if (dst->files[fd])
            dst->files[fd]->refs++;     /* fork: both share the open file */
    }
}

void fd_close_all(struct process *p)
{
    for (int fd = 0; fd < FD_MAX; fd++) {
        if (p->files[fd]) {
            file_put(p->files[fd]);
            p->files[fd] = NULL;
        }
    }
}
