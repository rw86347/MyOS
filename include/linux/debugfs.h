#ifndef LINUX_DEBUGFS_H
#define LINUX_DEBUGFS_H
/* No debugfs: creating an entry gives back an error pointer, which every
   caller is written to accept. */
#include <linux/err.h>
#include <linux/types.h>
struct dentry;
struct file_operations;
#define debugfs_create_dir(n, p)           ((struct dentry *)ERR_PTR(-ENODEV))
#define debugfs_create_file(n, m, p, d, f) ((struct dentry *)ERR_PTR(-ENODEV))
#define debugfs_create_symlink(n, p, t)    ((struct dentry *)ERR_PTR(-ENODEV))
#define debugfs_create_bool(n, m, p, v)    do { } while (0)
#define debugfs_create_u8(n, m, p, v)      do { } while (0)
#define debugfs_create_u16(n, m, p, v)     do { } while (0)
#define debugfs_create_u32(n, m, p, v)     do { } while (0)
#define debugfs_create_x32(n, m, p, v)     do { } while (0)
#define debugfs_remove(d)                  do { } while (0)
#define debugfs_remove_recursive(d)        do { } while (0)
#endif
