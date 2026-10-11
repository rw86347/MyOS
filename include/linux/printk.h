/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef LINUX_PRINTK_H
#define LINUX_PRINTK_H

/* printk and the pr_* family. A message may start with a level ("\001" "3"
   for KERN_ERR); errors and warnings go to the screen, info to the log
   (serial), debug nowhere — debug messages cost nothing. */
#include <linux/compiler.h>
#include <stdarg.h>

/* A format and its arguments, passed on whole: printk("%pV", &vaf). */
struct va_format { const char *fmt; va_list *va; };

#define KERN_SOH     "\001"
#define KERN_EMERG   KERN_SOH "0"
#define KERN_ALERT   KERN_SOH "1"
#define KERN_CRIT    KERN_SOH "2"
#define KERN_ERR     KERN_SOH "3"
#define KERN_WARNING KERN_SOH "4"
#define KERN_NOTICE  KERN_SOH "5"
#define KERN_INFO    KERN_SOH "6"
#define KERN_DEBUG   KERN_SOH "7"
#define KERN_CONT    KERN_SOH "c"

int printk(const char *fmt, ...) __printf(1, 2);
#ifndef pr_fmt
#define pr_fmt(fmt) fmt
#endif
#define pr_emerg(fmt, ...)   printk(KERN_EMERG pr_fmt(fmt), ##__VA_ARGS__)
#define pr_alert(fmt, ...)   printk(KERN_ALERT pr_fmt(fmt), ##__VA_ARGS__)
#define pr_crit(fmt, ...)    printk(KERN_CRIT pr_fmt(fmt), ##__VA_ARGS__)
#define pr_err(fmt, ...)     printk(KERN_ERR pr_fmt(fmt), ##__VA_ARGS__)
#define pr_warn(fmt, ...)    printk(KERN_WARNING pr_fmt(fmt), ##__VA_ARGS__)
#define pr_notice(fmt, ...)  printk(KERN_NOTICE pr_fmt(fmt), ##__VA_ARGS__)
#define pr_info(fmt, ...)    printk(KERN_INFO pr_fmt(fmt), ##__VA_ARGS__)
#define pr_cont(fmt, ...)    printk(KERN_CONT fmt, ##__VA_ARGS__)
#define pr_debug(fmt, ...)   do { if (0) printk(KERN_DEBUG pr_fmt(fmt), ##__VA_ARGS__); } while (0)
#define pr_devel(fmt, ...)   pr_debug(fmt, ##__VA_ARGS__)
#define pr_err_once(fmt, ...)  ({ static int _o; if (!_o) { _o = 1; pr_err(fmt, ##__VA_ARGS__); } })
#define pr_warn_once(fmt, ...) ({ static int _o; if (!_o) { _o = 1; pr_warn(fmt, ##__VA_ARGS__); } })
#define pr_info_once(fmt, ...) ({ static int _o; if (!_o) { _o = 1; pr_info(fmt, ##__VA_ARGS__); } })
#define pr_err_ratelimited(fmt, ...)  pr_err(fmt, ##__VA_ARGS__)
#define pr_warn_ratelimited(fmt, ...) pr_warn(fmt, ##__VA_ARGS__)
#define pr_info_ratelimited(fmt, ...) pr_info(fmt, ##__VA_ARGS__)
#define printk_ratelimit()   1
#define net_ratelimit()      1
#define no_printk(fmt, ...)  do { if (0) printk(fmt, ##__VA_ARGS__); } while (0)
/* Where we were called from (no unwinder: the caller's address only). */
void dump_stack(void);
void print_hex_dump(const char *level, const char *prefix, int type, int rowsize,
                    int groupsize, const void *buf, unsigned long len, int ascii);
#define DUMP_PREFIX_NONE    0
#define DUMP_PREFIX_ADDRESS 1
#define DUMP_PREFIX_OFFSET  2

#endif
