/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef TTY_H
#define TTY_H

#include <stdint.h>

struct file;

/* The terminal (tty.c): keyboard in, screen out, with line editing. */

/* Start taking keys from the keyboard (instead of the kernel's echo loop). */
void tty_attach_keyboard(void);

/* Bytes arriving from the keyboard (or a test). Interrupt-safe. */
void tty_input(const char *bytes, int n);

/* The console's size changed (vt.c): TIOCGWINSZ reports it, and the
   foreground programs get SIGWINCH. */
void tty_set_winsize(int rows, int cols);

/* Is a program waiting in read()? (For tests that type at it.) */
int tty_reader_waiting(void);

/* /dev/ttyS0 (major 4, minor 64, as on Linux): the serial port alone. */
#define TTYS0_RDEV ((4 << 8) | 64)

/* Has /dev/console been pointed at ttyS0 (ioctl TIOCCONS)? Then what
   programs write to it goes to the serial port, not the screen. */
int tty_console_on_serial(void);

/* The console's file operations use these. */
int32_t tty_read(struct file *f, void *user_buf, uint32_t n);
int32_t tty_ioctl(struct file *f, uint32_t cmd, uintptr_t arg);
uint32_t tty_poll(struct file *f);

/* The foreground process group: the one Ctrl-C signals (0 = none). */
int tty_foreground(void);
void tty_set_foreground(int pgid);

/* vhangup(): as if the line went dead. Typed input is thrown away and the
   foreground group gets SIGHUP (and SIGCONT). Open files stay usable:
   Linux would make them fail, so the next user can't be listened to. */
void tty_hangup(void);

/* --- the line discipline for other terminals (pty.c) --- */
struct tty;
typedef void (*tty_out_fn)(void *owner, const char *s, uint32_t n);

struct tty *tty_new(tty_out_fn out, void *owner);  /* echo goes to out */
void     tty_free(struct tty *t);
void     tty_feed(struct tty *t, const char *bytes, uint32_t n);    /* typing */
int32_t  tty_read_from(struct tty *t, struct file *f, void *user_buf, uint32_t n);
int32_t  tty_ioctl_on(struct tty *t, uint32_t cmd, uintptr_t arg); /* termios, size, pgrp */
/* Job control (SIGTTOU) for a program's own side of terminal t (NULL: the
   console), before a write or an ioctl: 0 to go ahead, -ERESTARTSYS once
   SIGTTOU has stopped a background job (the call runs again after `fg`),
   or -EIO / -ENOTTY. Not for a pty's master: that's the terminal window. */
int      tty_write_check(struct tty *t);
int      tty_ioctl_check(struct tty *t, uint32_t cmd);
int      tty_readable(struct tty *t);
uint32_t tty_space(struct tty *t);          /* bytes tty_feed takes without dropping */
void     tty_wait_space(struct tty *t);     /* sleep until a read makes room */
int      tty_onlcr(struct tty *t);          /* write '\n' as "\r\n"? */
void     tty_set_hung(struct tty *t);       /* other end gone: SIGHUP, then EOF */
int      tty_get_fg(struct tty *t);
void     tty_set_fg(struct tty *t, int pgid);
/* The session whose controlling terminal it is (0: none): a pty's from the
   session leader that took it, the console's from whoever sets its
   foreground group. Its other groups are background jobs. */
void     tty_set_session(struct tty *t, int sid);

#endif
