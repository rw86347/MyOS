/*
 * tcp.c — TCP connections, client side (step 10)
 *
 * TCP is a conversation with sequence numbers. We pick a starting number
 * and send SYN. The peer replies SYN+ACK, echoing our number plus one, and
 * gives us its own starting number. We ACK that. After that, each byte
 * (and each SYN or FIN) consumes one number, so both sides know what has
 * arrived.
 *
 * Each connection has a 16 KiB ring each way. write() copies into the send
 * ring and sends what the peer's window and TCP_INFLIGHT allow; the rest
 * goes out as ACKs come back. An unacked segment is resent from snd_una
 * after the retransmit timeout, which doubles each time (250 ms .. 4 s);
 * after 8 tries the connection fails with ETIMEDOUT.
 *
 * Bounded everywhere: TCP_MAX connections, fixed rings, segments built in
 * one static buffer, lookups scan TCP_MAX slots. That one buffer is only
 * safe because ip_send copies the bytes out and never blocks: if sending
 * could sleep, another thread would reach emit() and overwrite the segment
 * before it reached the card. Out-of-order data is dropped (the peer
 * resends it); we answer with a duplicate ACK so it can resend fast.
 *
 * Not yet: listen/accept, congestion control (slow start), SACK, window
 * scaling, timestamps, a full TIME_WAIT (we hold it 2 s, not 60).
 */
#include <errno.h>
#include <interrupts.h>
#include <ipv4.h>
#include <kheap.h>
#include <netdevice.h>
#include <random.h>
#include <signal.h>
#include <stddef.h>
#include <string.h>
#include <tcp.h>
#include <timer.h>
#include <uaccess.h>

#define ST_CLOSED     0
#define ST_SYN_SENT   1
#define ST_EST        2
#define ST_FIN_WAIT1  3
#define ST_FIN_WAIT2  4
#define ST_CLOSING    5
#define ST_TIME_WAIT  6
#define ST_CLOSE_WAIT 7
#define ST_LAST_ACK   8

#define FL_FIN 0x01
#define FL_SYN 0x02
#define FL_RST 0x04
#define FL_PSH 0x08
#define FL_ACK 0x10

#define TCP_MAX      16
#define RXBUF        16384
#define TXBUF        16384
#define MSS_MAX      1460           /* Ethernet: 1500 - IP - TCP headers */
#define MSS_DEFAULT  536            /* if the peer sends no MSS option */
#define TCP_INFLIGHT 4              /* segments sent but not acked */
#define RTO_MIN      250
#define RTO_MAX      4000
#define RETRIES      8
#define PERSIST_MAX  60000          /* a shut window backs off this far */
#define PROBES       10             /* probes at a shut window before giving up */
#define TIME_WAIT_MS 2000
#define ORPHAN_MS    10000          /* a closed socket's FIN gets this long */

struct tcp_conn {
    int state;
    int orphan;                     /* the socket is gone */
    int err;                        /* -errno to report, 0 = none */
    uint32_t remote;
    uint16_t lport, rport, mss;
    uint32_t snd_una, snd_nxt, snd_wnd, rcv_nxt;
    int got_fin, fin_queued, fin_sent;
    uint32_t rto, rto_at, retries;  /* rto_at: 0 = timer off */
    uint32_t end_at;                /* TIME_WAIT / orphan deadline, 0 = none */
    uint32_t rx_head, rx_len;
    uint32_t tx_head, tx_len, tx_sent;  /* tx_sent: bytes of tx_len on the wire */
    uint8_t rx[RXBUF];
    uint8_t tx[TXBUF];
};

static struct tcp_conn *conns[TCP_MAX];
static uint8_t seg[24 + MSS_MAX];   /* the one segment being built */

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static int seq_lt(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }
static int due(uint32_t at, uint32_t now) { return at && (int32_t)(now - at) >= 0; }

/* --- sending ------------------------------------------------------------ */

static uint16_t rx_window(struct tcp_conn *c)
{
    return (uint16_t)(RXBUF - c->rx_len);   /* RXBUF < 64 KiB: no scaling */
}

/* Build and send one segment. Data (len bytes) comes from the send ring
   at offset `off` from tx_head; SYN carries our MSS option. */
static int emit(struct tcp_conn *c, uint8_t flags, uint32_t seq, uint32_t off, uint32_t len)
{
    uint32_t hlen = (flags & FL_SYN) ? 24 : 20;
    memset(seg, 0, hlen);
    wr16(seg, c->lport);
    wr16(seg + 2, c->rport);
    wr32(seg + 4, seq);
    if (flags & FL_ACK)
        wr32(seg + 8, c->rcv_nxt);
    seg[12] = (uint8_t)((hlen / 4) << 4);
    seg[13] = flags;
    wr16(seg + 14, rx_window(c));
    if (flags & FL_SYN) {
        seg[20] = 2;                /* option: MSS, 4 bytes */
        seg[21] = 4;
        wr16(seg + 22, MSS_MAX);
    }
    if (len) {
        uint32_t at = (c->tx_head + off) % TXBUF;
        uint32_t first = TXBUF - at < len ? TXBUF - at : len;
        memcpy(seg + hlen, c->tx + at, first);
        memcpy(seg + hlen + first, c->tx, len - first);
    }
    uint16_t sum = ip_transport_csum(ipv4_addr, c->remote, IPPROTO_TCP, seg, hlen + len);
    wr16(seg + 16, sum);
    return ip_send(c->remote, IPPROTO_TCP, seg, hlen + len);
}

static void send_ack(struct tcp_conn *c)
{
    emit(c, FL_ACK, c->snd_nxt, 0, 0);
}

static void arm(struct tcp_conn *c)
{
    if (!c->rto_at)
        c->rto_at = (timer_ms() + c->rto) | 1;      /* never 0: 0 means off */
}

/* Send whatever the window allows: new data, then the FIN once the send
   ring is empty. */
static void output(struct tcp_conn *c)
{
    if (c->state != ST_EST && c->state != ST_CLOSE_WAIT)
        return;
    uint32_t limit = c->snd_wnd < TCP_INFLIGHT * c->mss ? c->snd_wnd : TCP_INFLIGHT * c->mss;
    /*
     * A shut window would otherwise stop the connection dead: nothing can be
     * sent, so arm() below is never reached, so no timer runs and nothing ever
     * tries again. One byte is allowed past the window to probe it (RFC 1122
     * 4.2.2.17). The peer drops that byte and answers with its current window,
     * which is how we learn the window reopened even if the update saying so
     * was lost. Once the byte is on the wire the retransmit timer repeats it,
     * and that repetition is the persist timer.
     */
    if (!limit && !c->tx_sent && c->tx_len)
        limit = 1;
    while (c->tx_sent < c->tx_len && c->tx_sent < limit) {
        uint32_t n = c->tx_len - c->tx_sent;
        if (n > c->mss)
            n = c->mss;
        if (n > limit - c->tx_sent)
            n = limit - c->tx_sent;
        if (emit(c, FL_ACK | FL_PSH, c->snd_una + c->tx_sent, c->tx_sent, n) < 0)
            break;                  /* card full: the timer resends */
        c->tx_sent += n;
        c->snd_nxt = c->snd_una + c->tx_sent;
        arm(c);
    }
    if (c->fin_queued && !c->fin_sent && c->tx_sent == c->tx_len) {
        emit(c, FL_FIN | FL_ACK, c->snd_nxt, 0, 0);
        c->fin_sent = 1;
        c->snd_nxt++;
        c->state = c->state == ST_EST ? ST_FIN_WAIT1 : ST_LAST_ACK;
        arm(c);
    }
    /* Anything left waiting needs a timer behind it, even when not one byte
       made it out: the probe above can fail too (the card's ring was full),
       and a send with no timer is a connection that stops here for good. */
    if (c->tx_sent < c->tx_len || (c->fin_queued && !c->fin_sent))
        arm(c);
}

/* Answer a segment for no connection of ours. */
/* The reset for a segment that belongs to no connection of ours. Sent to
   whoever the source address claims to be, so it is a reflector like an echo
   reply, and it gets the same per-peer budget. */
static void send_rst(uint32_t to, uint16_t lport, uint16_t rport,
                     uint32_t seq, uint32_t ack, uint8_t flags)
{
    if (!ip_rate_ok(to))
        return;
    memset(seg, 0, 20);
    wr16(seg, lport);
    wr16(seg + 2, rport);
    wr32(seg + 4, seq);
    wr32(seg + 8, ack);
    seg[12] = 5 << 4;
    seg[13] = flags;
    wr16(seg + 16, ip_transport_csum(ipv4_addr, to, IPPROTO_TCP, seg, 20));
    ip_send(to, IPPROTO_TCP, seg, 20);
}

/* --- connections -------------------------------------------------------- */

static void conn_free(struct tcp_conn *c)
{
    for (int i = 0; i < TCP_MAX; i++)
        if (conns[i] == c)
            conns[i] = 0;
    kfree(c);
}

static void fail(struct tcp_conn *c, int err)
{
    c->err = err;
    c->state = ST_CLOSED;
    c->rto_at = 0;
}

static int port_used(uint16_t p)
{
    for (int i = 0; i < TCP_MAX; i++)
        if (conns[i] && conns[i]->lport == p)
            return 1;
    return 0;
}

/*
 * The initial sequence number, RFC 6528: ISN = M + F(four-tuple, secret).
 *
 * M is a timer ticking every 4 microseconds, so numbers still climb with time
 * the way a peer holding a stale connection from a previous incarnation of
 * this four-tuple expects. F is a keyed hash, so the offset is different and
 * unrelated for every four-tuple and an attacker who sees one connection's
 * sequence number learns nothing about the next.
 *
 * What this replaces was `timer_ms() * 64000 + 0x10000000` — the same number
 * for every connection at a given millisecond, derived from a clock the
 * attacker can read by other means. Knowing the sequence space is what lets
 * an off-path attacker inject data into or reset a connection blind, so this
 * was the RFC 6528 attack with nothing left to work out.
 */
static uint32_t isn(uint16_t lport, uint32_t remote, uint16_t rport)
{
    struct { uint32_t l, r; uint16_t lp, rp; } t = { ipv4_addr, remote, lport, rport };
    /* No padding, or the padding bytes would be unspecified and the same
       four-tuple could hash two ways — which RFC 6528 relies on not happening. */
    _Static_assert(sizeof t == 12, "the four-tuple must pack");
    return random_keyed(&t, sizeof t) + timer_ms() * 250u;   /* 250 per ms = 4 us */
}

struct tcp_conn *tcp_open(void)
{
    int slot = -1;
    for (int i = 0; i < TCP_MAX && slot < 0; i++)
        if (!conns[i])
            slot = i;
    if (slot < 0)
        return 0;
    struct tcp_conn *c = kmalloc(sizeof *c);
    if (!c)
        return 0;
    memset(c, 0, offsetof(struct tcp_conn, rx));
    c->mss = MSS_DEFAULT;
    c->rto = RTO_MIN;
    conns[slot] = c;
    return c;
}

/* Sleep a little; the e1000 interrupt or the cap ends it. */
static int nap(int nonblock, uint32_t deadline)
{
    if (nonblock)
        return -EAGAIN;
    if (signal_pending())
        return -EINTR;
    if (deadline && due(deadline, timer_ms()))
        return -ETIMEDOUT;
    net_sleep(50);
    net_rx_action();
    tcp_timers(timer_ms());
    return 0;
}

int tcp_connect(struct tcp_conn *c, uint32_t dst, uint16_t port, int nonblock)
{
    if (c->state == ST_SYN_SENT)
        return nonblock ? -EALREADY : -EINPROGRESS;
    if (c->state != ST_CLOSED || c->lport)
        return -EISCONN;
    if (!dst || !port)
        return -EINVAL;
    c->lport = ip_ephemeral_port(port_used);
    if (!c->lport)
        return -EADDRNOTAVAIL;
    c->remote = dst;
    c->rport = port;
    uint32_t iss = isn(c->lport, dst, port);
    c->snd_una = iss;
    c->snd_nxt = iss + 1;
    c->state = ST_SYN_SENT;
    if (emit(c, FL_SYN, iss, 0, 0) < 0) {
        fail(c, -ENETUNREACH);      /* ARP got no answer */
        return c->err;
    }
    arm(c);
    if (nonblock)
        return -EINPROGRESS;
    while (c->state == ST_SYN_SENT) {
        int rc = nap(0, 0);
        if (rc < 0)
            return rc;              /* EINTR: the handshake goes on */
    }
    if (c->state == ST_CLOSED) {
        int e = c->err;
        c->err = 0;
        return e ? e : -ECONNREFUSED;
    }
    return 0;
}

uint32_t tcp_buf_max(void)
{
    return TXBUF > RXBUF ? TXBUF : RXBUF;
}

int tcp_write(struct tcp_conn *c, const void *src, uint32_t len, int user, int nonblock)
{
    if (!len)
        return 0;
    for (;;) {
        if (c->err && c->state == ST_CLOSED)
            return c->err;
        if (c->state == ST_SYN_SENT) {
            int rc = nap(nonblock, 0);
            if (rc < 0)
                return rc;
            continue;
        }
        if ((c->state != ST_EST && c->state != ST_CLOSE_WAIT) || c->fin_queued)
            return -EPIPE;
        if (c->tx_len < TXBUF)
            break;
        net_rx_action();            /* ACKs may be waiting in the card's ring */
        if (c->tx_len < TXBUF)
            continue;
        int rc = nap(nonblock, 0);
        if (rc < 0)
            return rc;
    }
    /* room, not TXBUF - tx_len: copy_from_user can sleep (it may demand-page
       an mmap'd file), so a second writer on the same socket -- a fork shares
       it -- can run between the length being read here and being committed
       below. If tx_len has overshot TXBUF, the subtraction would wrap to
       about four billion, stop clamping n, and run the copy off the end of a
       16 KiB array inside the connection with as much data as the caller
       liked. Written this way the raced case copies nothing instead. */
    uint32_t room = c->tx_len < TXBUF ? TXBUF - c->tx_len : 0;
    uint32_t n = room < len ? room : len;
    uint32_t at = (c->tx_head + c->tx_len) % TXBUF;
    uint32_t first = TXBUF - at < n ? TXBUF - at : n;
    c->tx_len += n;                 /* claim it before the copy can sleep */
    uint32_t claimed = c->tx_len;
    if (user) {
        if (copy_from_user(c->tx + at, src, first) < 0 ||
            copy_from_user(c->tx, (const uint8_t *)src + first, n - first) < 0) {
            if (c->tx_len == claimed)
                c->tx_len -= n;     /* nobody else claimed after us: give it back */
            else {
                memset(c->tx + at, 0, first);    /* do not put stale kernel */
                memset(c->tx, 0, n - first);     /* bytes on the wire */
            }
            return -EFAULT;
        }
    } else {
        memcpy(c->tx + at, src, first);
        memcpy(c->tx, (const uint8_t *)src + first, n - first);
    }
    output(c);
    return (int)n;
}

int tcp_read(struct tcp_conn *c, void *dst, uint32_t len, int user, int nonblock,
             int peek, uint32_t timeout_ms)
{
    if (!len)
        return 0;
    uint32_t deadline = timeout_ms ? (timer_ms() + timeout_ms) | 1 : 0;
    if (!c->rx_len)
        net_rx_action();            /* it may be waiting in the card's ring */
    while (!c->rx_len) {
        if (c->got_fin)
            return 0;
        if (c->state == ST_CLOSED)
            return c->err ? c->err : (c->lport ? 0 : -ENOTCONN);
        int rc = nap(nonblock, deadline);
        if (rc < 0)
            return rc;
    }
    /* Clamped to RXBUF as well as to rx_len, and consumed before the copy:
       copying can sleep, so a second reader on the same socket can run in
       between, and two readers that both subtract an rx_len they both read
       drive it below zero. The clamp keeps that from turning into a read off
       the end of the connection's 16 KiB buffer, and consuming first keeps
       it from happening. A peek cannot consume, so it relies on the clamp. */
    uint32_t avail = c->rx_len < RXBUF ? c->rx_len : RXBUF;
    uint32_t n = avail < len ? avail : len;
    uint32_t first = RXBUF - c->rx_head < n ? RXBUF - c->rx_head : n;
    uint32_t before = rx_window(c), head = c->rx_head;
    if (!peek) {
        c->rx_head = (c->rx_head + n) % RXBUF;
        c->rx_len -= n;
    }
    if (user) {
        if (copy_to_user(dst, c->rx + head, first) < 0 ||
            copy_to_user((uint8_t *)dst + first, c->rx, n - first) < 0)
            return -EFAULT;
    } else {
        memcpy(dst, c->rx + head, first);
        memcpy((uint8_t *)dst + first, c->rx, n - first);
    }
    if (peek)
        return (int)n;
    if (before < RXBUF / 2 && rx_window(c) >= RXBUF / 2 && c->state != ST_CLOSED)
        send_ack(c);                /* window update: it had nearly closed */
    return (int)n;
}

unsigned tcp_poll(struct tcp_conn *c)
{
    unsigned r = 0;
    if (c->rx_len || c->got_fin || c->state == ST_CLOSED)
        r |= TCP_READABLE;
    if (c->state == ST_CLOSED ||
        ((c->state == ST_EST || c->state == ST_CLOSE_WAIT) && c->tx_len < TXBUF))
        r |= TCP_WRITABLE;
    return r;
}

uint32_t tcp_rx_avail(struct tcp_conn *c) { return c->rx_len; }
int tcp_connected(struct tcp_conn *c) { return c->state != ST_CLOSED && c->state != ST_SYN_SENT; }

int tcp_error(struct tcp_conn *c, int clear)
{
    int e = c->err;
    if (clear)
        c->err = 0;
    return e;
}

void tcp_names(struct tcp_conn *c, uint16_t *lport, uint32_t *raddr, uint16_t *rport)
{
    *lport = c->lport;
    *raddr = c->remote;
    *rport = c->rport;
}

void tcp_shutdown_write(struct tcp_conn *c)
{
    if (c->state == ST_EST || c->state == ST_CLOSE_WAIT) {
        c->fin_queued = 1;
        output(c);
    }
}

void tcp_release(struct tcp_conn *c)
{
    if (c->state == ST_CLOSED || c->state == ST_SYN_SENT) {
        conn_free(c);
        return;
    }
    c->orphan = 1;
    c->end_at = (timer_ms() + ORPHAN_MS) | 1;
    if (c->rx_len) {                /* unread data: reset, as Linux does */
        emit(c, FL_RST | FL_ACK, c->snd_nxt, 0, 0);
        conn_free(c);
        return;
    }
    tcp_shutdown_write(c);
}

int tcp_flush(struct tcp_conn *c, uint32_t timeout_ms)
{
    uint32_t deadline = (timer_ms() + timeout_ms) | 1;
    while (c->state != ST_CLOSED && c->snd_una != c->snd_nxt) {
        int rc = nap(0, deadline);
        if (rc < 0)
            return rc;
    }
    return c->state == ST_CLOSED && c->err ? c->err : 0;
}

/* --- receiving ---------------------------------------------------------- */

static struct tcp_conn *lookup(uint32_t src, uint16_t sport, uint16_t dport)
{
    for (int i = 0; i < TCP_MAX; i++) {
        struct tcp_conn *c = conns[i];
        if (c && c->state != ST_CLOSED && c->lport == dport &&
            c->rport == sport && c->remote == src)
            return c;
    }
    return 0;
}

static uint16_t peer_mss(const uint8_t *opt, uint32_t n)
{
    uint32_t i = 0;
    while (i < n) {
        if (opt[i] == 0)
            break;                  /* end of options */
        if (opt[i] == 1) {          /* no-op */
            i++;
            continue;
        }
        if (i + 1 >= n || opt[i + 1] < 2 || i + opt[i + 1] > n)
            break;                  /* malformed: stop looking */
        if (opt[i] == 2 && opt[i + 1] == 4)
            return (uint16_t)((opt[i + 2] << 8) | opt[i + 3]);
        i += opt[i + 1];
    }
    return MSS_DEFAULT;
}

/* The peer acked up to `ack`: drop that from the send ring. */
static void take_ack(struct tcp_conn *c, uint32_t ack)
{
    if (seq_lt(ack, c->snd_una) || seq_lt(c->snd_nxt, ack) || ack == c->snd_una)
        return;
    uint32_t n = ack - c->snd_una;
    uint32_t d = n < c->tx_sent ? n : c->tx_sent;
    c->tx_head = (c->tx_head + d) % TXBUF;
    c->tx_len -= d;
    c->tx_sent -= d;
    c->snd_una += d;
    int fin_acked = n > d;          /* the only other thing in flight */
    if (fin_acked)
        c->snd_una++;
    c->retries = 0;
    c->rto = RTO_MIN;
    c->rto_at = 0;
    if (c->snd_una != c->snd_nxt)
        arm(c);
    if (fin_acked) {
        if (c->state == ST_FIN_WAIT1)
            c->state = ST_FIN_WAIT2;
        else if (c->state == ST_CLOSING) {
            c->state = ST_TIME_WAIT;
            c->end_at = (timer_ms() + TIME_WAIT_MS) | 1;
        } else if (c->state == ST_LAST_ACK)
            c->state = ST_CLOSED;
    }
}

void tcp_input(uint32_t src, uint32_t dst, const uint8_t *pkt, uint32_t len)
{
    (void)dst;
    if (len < 20 || ip_transport_csum(src, ipv4_addr, IPPROTO_TCP, pkt, len) != 0)
        return;
    uint32_t hlen = (uint32_t)(pkt[12] >> 4) * 4;
    if (hlen < 20 || hlen > len)
        return;
    uint16_t sport = (uint16_t)((pkt[0] << 8) | pkt[1]);
    uint16_t dport = (uint16_t)((pkt[2] << 8) | pkt[3]);
    uint8_t flags = pkt[13];
    uint32_t seq = rd32(pkt + 4);
    uint32_t ack = rd32(pkt + 8);
    uint32_t wnd = (uint32_t)((pkt[14] << 8) | pkt[15]);
    uint32_t dlen = len - hlen;

    struct tcp_conn *c = lookup(src, sport, dport);
    if (!c) {
        if (flags & FL_RST)
            return;
        if (flags & FL_ACK)
            send_rst(src, dport, sport, ack, 0, FL_RST);
        else
            send_rst(src, dport, sport, 0,
                     seq + dlen + !!(flags & FL_SYN) + !!(flags & FL_FIN), FL_RST | FL_ACK);
        return;
    }

    if (flags & FL_RST) {
        /* Only a reset that fits exactly counts (RFC 5961): a blind attacker
           would have to guess the sequence number. */
        int ok = c->state == ST_SYN_SENT ? ((flags & FL_ACK) && ack == c->snd_nxt)
                                         : seq == c->rcv_nxt;
        if (ok) {
            fail(c, c->state == ST_SYN_SENT ? -ECONNREFUSED : -ECONNRESET);
            if (c->orphan)
                conn_free(c);
        }
        return;
    }

    if (c->state == ST_SYN_SENT) {
        if ((flags & (FL_SYN | FL_ACK)) != (FL_SYN | FL_ACK) || ack != c->snd_nxt)
            return;
        uint16_t m = peer_mss(pkt + 20, hlen - 20);
        c->mss = m < 64 ? 64 : m > MSS_MAX ? MSS_MAX : m;
        c->rcv_nxt = seq + 1;
        c->snd_una = ack;
        c->snd_wnd = wnd;
        c->rto_at = 0;
        c->retries = 0;
        c->state = ST_EST;
        send_ack(c);
        output(c);
        return;
    }

    if (flags & FL_SYN) {           /* their SYN+ACK again: our ACK was lost */
        send_ack(c);
        return;
    }
    if (!(flags & FL_ACK))
        return;
    int was_shut = !c->snd_wnd;
    take_ack(c, ack);
    c->snd_wnd = wnd;
    /* A window that reopens is a sign of life even when the ack acknowledges
       nothing new (take_ack ignores those). The backoff and probe count that
       were covering a shut window must not be charged against the data that
       can now flow, or the next timer would kill a healthy connection. */
    if (was_shut && wnd) {
        c->retries = 0;
        c->rto = RTO_MIN;
        c->rto_at = 0;
        if (c->snd_una != c->snd_nxt)
            arm(c);
    }
    if (c->state == ST_CLOSED) {    /* LAST_ACK done */
        if (c->orphan)
            conn_free(c);
        return;
    }

    int need_ack = 0;
    if (dlen) {
        if (c->got_fin) {
            /* nothing more can come after their FIN */
        } else if (c->orphan) {
            emit(c, FL_RST | FL_ACK, c->snd_nxt, 0, 0);
            conn_free(c);
            return;
        } else if (seq == c->rcv_nxt) {
            uint32_t room = RXBUF - c->rx_len;
            uint32_t n = dlen < room ? dlen : room;
            uint32_t at = (c->rx_head + c->rx_len) % RXBUF;
            uint32_t first = RXBUF - at < n ? RXBUF - at : n;
            memcpy(c->rx + at, pkt + hlen, first);
            memcpy(c->rx, pkt + hlen + first, n - first);
            c->rx_len += n;
            c->rcv_nxt += n;
        }
        need_ack = 1;               /* new data, a duplicate, or a gap */
    }
    if ((flags & FL_FIN) && !c->got_fin && seq + dlen == c->rcv_nxt) {
        c->rcv_nxt++;
        c->got_fin = 1;
        need_ack = 1;
        if (c->state == ST_EST)
            c->state = ST_CLOSE_WAIT;
        else if (c->state == ST_FIN_WAIT1)
            c->state = ST_CLOSING;
        else if (c->state == ST_FIN_WAIT2) {
            c->state = ST_TIME_WAIT;
            c->end_at = (timer_ms() + TIME_WAIT_MS) | 1;
        }
    }
    if (need_ack)
        send_ack(c);
    output(c);
}

/* --- timers -------------------------------------------------------------- */

uint32_t tcp_timers(uint32_t now)
{
    uint32_t next = 0;
    for (int i = 0; i < TCP_MAX; i++) {
        struct tcp_conn *c = conns[i];
        if (!c)
            continue;
        if (due(c->end_at, now) &&
            (c->orphan || c->state == ST_TIME_WAIT)) {
            if (c->orphan) {
                conn_free(c);
                continue;
            }
            c->state = ST_CLOSED;   /* TIME_WAIT over; the socket still holds it */
            c->end_at = 0;
        }
        if (c->orphan && c->state == ST_CLOSED) {
            conn_free(c);
            continue;
        }
        if (due(c->rto_at, now)) {
            /*
             * A shut window is not a lost packet. The peer is alive and its
             * application has stopped reading, which can legitimately last
             * minutes, so probes get a longer leash and back off further than
             * retransmits do. Bounded all the same: RFC 1122 says to keep
             * probing for as long as the peer answers, but code that never
             * gives up is a hang, and a hang is what the attacker wanted.
             */
            int probing = !c->snd_wnd && c->tx_sent && c->state != ST_SYN_SENT;
            if (++c->retries > (probing ? PROBES : RETRIES)) {
                fail(c, -ETIMEDOUT);
                if (c->orphan) {
                    conn_free(c);
                    continue;
                }
            } else {
                if (c->state == ST_SYN_SENT)
                    emit(c, FL_SYN, c->snd_una, 0, 0);
                else if (c->tx_sent)
                    emit(c, FL_ACK | FL_PSH, c->snd_una, 0,
                         c->tx_sent < c->mss ? c->tx_sent : c->mss);
                else if (c->fin_sent)
                    emit(c, FL_FIN | FL_ACK, c->snd_nxt - 1, 0, 0);
                uint32_t cap = probing ? PERSIST_MAX : RTO_MAX;
                c->rto = c->rto * 2 > cap ? cap : c->rto * 2;
                c->rto_at = (now + c->rto) | 1;
            }
        }
        uint32_t t[2] = { c->rto_at, c->end_at };
        for (int k = 0; k < 2; k++)
            if (t[k] && (!next || seq_lt(t[k], next)))
                next = t[k];
    }
    return next;
}
