/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef PTY_H
#define PTY_H

/* Pseudo-terminals (drivers/pty.c): /dev/ptmx (5,2) makes a pair, the
   slave is /dev/pts/N (136,N). */

struct file;
struct file_ops;
struct inode;

#define PTY_SLAVE_MAJOR 136

void pty_init(struct inode *dev);           /* makes /dev/pts */
const struct file_ops *pty_master_ops(void);
const struct file_ops *pty_slave_ops(void);

/* open() of /dev/tty: if the caller's session has a pty as its
   controlling terminal, f becomes that slave (1); else 0. */
int pty_open_ctty(struct file *f);

#endif
