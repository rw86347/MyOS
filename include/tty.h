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
int      tty_readable(struct tty *t);
uint32_t tty_space(struct tty *t);          /* bytes tty_feed takes without dropping */
void     tty_wait_space(struct tty *t);     /* sleep until a read makes room */
int      tty_onlcr(struct tty *t);          /* write '\n' as "\r\n"? */
void     tty_set_hung(struct tty *t);       /* other end gone: SIGHUP, then EOF */
int      tty_get_fg(struct tty *t);
void     tty_set_fg(struct tty *t, int pgid);

#endif
