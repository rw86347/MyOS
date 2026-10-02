#ifndef TTY_H
#define TTY_H

#include <stdint.h>

struct file;

/* The terminal (tty.c): keyboard in, screen out, with line editing. */

/* Start taking keys from the keyboard (instead of the kernel's echo loop). */
void tty_attach_keyboard(void);

/* Bytes arriving from the keyboard (or a test). Interrupt-safe. */
void tty_input(const char *bytes, int n);

/* Is a program waiting in read()? (For tests that type at it.) */
int tty_reader_waiting(void);

/* The console's file operations use these. */
int32_t tty_read(struct file *f, void *user_buf, uint32_t n);
int32_t tty_ioctl(struct file *f, uint32_t cmd, uint32_t arg);
uint32_t tty_poll(struct file *f);

/* The foreground process group: the one Ctrl-C signals (0 = none). */
int tty_foreground(void);
void tty_set_foreground(int pgid);

#endif
