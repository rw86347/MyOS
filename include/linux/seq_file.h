#ifndef LINUX_SEQ_FILE_H
#define LINUX_SEQ_FILE_H
/* Only debugfs files use seq_file, and there is no debugfs. */
#include <linux/types.h>
struct seq_file { void *private; };
#define seq_printf(m, fmt, ...) do { (void)(m); } while (0)
#define seq_puts(m, s)          do { (void)(m); } while (0)
#endif
