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
int32_t tty_ioctl(struct file *f, uint32_t cmd, uint32_t arg);
uint32_t tty_poll(struct file *f);

/* The foreground process group: the one Ctrl-C signals (0 = none). */
int tty_foreground(void);
void tty_set_foreground(int pgid);

/* vhangup(): as if the line went dead. Typed input is thrown away and the
   foreground group gets SIGHUP (and SIGCONT). Open files stay usable:
   Linux would make them fail, so the next user can't be listened to. */
void tty_hangup(void);

#endif
