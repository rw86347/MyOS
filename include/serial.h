#ifndef DRIVERS_SERIAL_H
#define DRIVERS_SERIAL_H

#include <stddef.h>

/* COM1. Output is polled until serial_irq_init(), then queued and sent by
   the UART's interrupt in the background. */
void serial_init(void);
void serial_irq_init(void);         /* after idt_init() */

/* Send n bytes ('\n' becomes CR+LF). Returns at once once interrupt-driven. */
void serial_write_n(const char *s, size_t n);

/* Free room in the output queue, in bytes ('\n' takes two). */
size_t serial_space(void);

/* Send everything still queued, by polling (crashes, switching off). */
void serial_flush(void);

#endif
