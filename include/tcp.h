#ifndef TCP_H
#define TCP_H

#include <stdint.h>

/*
 * TCP connections, client side (step 10). Each socket owns one. Data is
 * buffered both ways, so write() returns once the bytes are copied in;
 * the network thread (net_thread in netdevice.c) runs retransmits and
 * receive while no program is in the kernel. No listen, no out-of-order
 * queue, no congestion control: at most TCP_INFLIGHT segments unacked.
 */

struct tcp_conn;

/* A new, closed connection, or NULL (out of memory or connections). */
struct tcp_conn *tcp_open(void);

/* Start the handshake. 0 when it is done; -EINPROGRESS if nonblock (then
   poll for POLLOUT and read the result with tcp_error). -errno on failure. */
int tcp_connect(struct tcp_conn *c, uint32_t dst, uint16_t port, int nonblock);

/* Queue up to len bytes. user: src is a user pointer. Returns bytes taken
   (>0), or -EAGAIN (nonblock, buffer full), -EPIPE, -EINTR, -EFAULT. */
int tcp_write(struct tcp_conn *c, const void *src, uint32_t len, int user, int nonblock);

/* Copy out up to len bytes. Returns the count, 0 at end of stream, or
   -EAGAIN, -EINTR, -ETIMEDOUT (timeout_ms, 0 = none), -ECONNRESET, -EFAULT. */
int tcp_read(struct tcp_conn *c, void *dst, uint32_t len, int user, int nonblock,
             int peek, uint32_t timeout_ms);

#define TCP_READABLE 1
#define TCP_WRITABLE 2
unsigned tcp_poll(struct tcp_conn *c);

uint32_t tcp_rx_avail(struct tcp_conn *c);
int  tcp_error(struct tcp_conn *c, int clear);  /* SO_ERROR */
int  tcp_connected(struct tcp_conn *c);          /* handshake done */

/* The most one tcp_read or tcp_write can move: the ring is the limit. The
   socket layer needs it to know how much of a user buffer is worth faulting
   in before a copy (see fault_in_user in uaccess.h). */
uint32_t tcp_buf_max(void);
void tcp_names(struct tcp_conn *c, uint16_t *lport, uint32_t *raddr, uint16_t *rport);

void tcp_shutdown_write(struct tcp_conn *c);     /* send FIN after the data */
/* The socket is gone. The connection finishes its FIN in the background
   and frees itself. */
void tcp_release(struct tcp_conn *c);

/* Kernel tests: wait until everything sent is acked (or error). */
int tcp_flush(struct tcp_conn *c, uint32_t timeout_ms);

void tcp_input(uint32_t src, uint32_t dst, const uint8_t *pkt, uint32_t len);

/* Retransmits and timeouts; returns the next timer_ms() it needs, 0 = none. */
uint32_t tcp_timers(uint32_t now);

#endif
