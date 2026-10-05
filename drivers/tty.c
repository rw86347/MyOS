/*
 * tty.c — the terminal: line editing between the keyboard and programs
 *
 * A program reading from the console doesn't read keys one by one. The
 * terminal's "line discipline" sits in between, and its settings (struct
 * termios, Linux's layout) say what it does:
 *
 *   canonical mode (ICANON, the default): the terminal edits the line —
 *       Backspace, Ctrl-U (kill line), Ctrl-W (kill word) — and a read()
 *       gets a whole line only when Enter is pressed. Ctrl-D on an empty
 *       line means "end of file". This is how `cat` or a simple shell read.
 *   raw mode (ICANON off): every byte is handed over at once and the
 *       program edits by itself — bash's readline and BusyBox's line
 *       editing work this way (arrow keys, history...).
 *   ECHO: typed characters appear on the screen (the terminal echoes them,
 *       not the program).
 *   ISIG: Ctrl-C interrupts the foreground program (SIGINT), Ctrl-\ quits.
 *
 * Input arrives in the keyboard interrupt and is processed right then; a
 * reader sleeps until there's a line (or enough bytes) and is woken.
 *
 * The same line discipline serves pseudo-terminals (pty.c): there the
 * "keyboard" is the master's writes and the "screen" is the master's reads.
 */
#include <errno.h>
#include <interrupts.h>
#include <keyboard.h>
#include <kheap.h>
#include <kprintf.h>
#include <proc.h>
#include <signal.h>
#include <string.h>
#include <tty.h>
#ifdef BENCH
#include <bench.h>
#endif
#include <uaccess.h>
#include <vfs.h>
#include <vt.h>

/* struct termios as the Linux kernel has it (36 bytes). */
#define NCCS 19
struct termios {
    uint32_t c_iflag, c_oflag, c_cflag, c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[NCCS];
};

/* c_iflag */
#define INLCR   0000100
#define IGNCR   0000200
#define ICRNL   0000400
#define IXON    0002000
/* c_oflag */
#define OPOST   0000001
#define ONLCR   0000004
/* c_cflag: 38400 baud, 8 bits, receiver on, hang up on close */
#define CFLAG_DEFAULT (0000017 | 0000060 | 0000200 | 0002000)
/* c_lflag */
#define ISIG    0000001
#define ICANON  0000002
#define ECHO    0000010
#define ECHOE   0000020
#define ECHOK   0000040
#define ECHONL  0000100
#define ECHOCTL 0001000
#define ECHOKE  0004000
#define IEXTEN  0100000
/* c_cc */
#define VINTR   0
#define VQUIT   1
#define VERASE  2
#define VKILL   3
#define VEOF    4
#define VTIME   5
#define VMIN    6
#define VSUSP  10
#define VEOL   11
#define VWERASE 14

/* ioctl requests */
#define TCGETS      0x5401
#define TCSETS      0x5402
#define TCSETSW     0x5403
#define TCSETSF     0x5404
#define TCFLSH      0x540B
#define TIOCSCTTY   0x540E
#define TIOCGPGRP   0x540F
#define TIOCSPGRP   0x5410
#define TIOCGWINSZ  0x5413
#define TIOCSWINSZ  0x5414
#define FIONREAD    0x541B
#define TIOCNOTTY   0x5422
#define KDSETMODE   0x4B3A          /* the console: text or graphics */
#define KDGETMODE   0x4B3B
#define KD_TEXT     0
#define KD_GRAPHICS 1
#define KDGKBTYPE   0x4B33
#define KB_101      2
#define KDGKBMODE   0x4B44          /* what keys send: */
#define KDSKBMODE   0x4B45
#define K_RAW       0               /*   scancodes */
#define K_XLATE     1               /*   characters (normal) */
#define K_MEDIUMRAW 2               /*   key codes */
#define K_UNICODE   3               /*   characters, UTF-8 */
#define K_OFF       4               /*   nothing (X reads /dev/input) */
#define VT_OPENQRY  0x5600          /* virtual terminals: */
#define VT_GETMODE  0x5601
#define VT_SETMODE  0x5602
#define VT_GETSTATE 0x5603
#define VT_RELDISP  0x5605
#define VT_ACTIVATE 0x5606
#define VT_WAITACTIVE 0x5607
#define VT_DISALLOCATE 0x5608

#define LINE_MAX  256               /* the line being edited */
#define IN_SIZE   2048              /* typed bytes waiting for a reader */
#define MAX_LINES 32                /* complete lines waiting */

/*
 * One terminal: its settings and what's been typed. The console is one
 * (the keyboard types at it, echo goes to the screen); each pty is another
 * (pty.c: the master's writes type at it, echo goes back to the master).
 * `out` is the only difference.
 */
struct tty {
    struct termios tio;
    char line[LINE_MAX];            /* canonical mode: the line being typed */
    uint32_t line_len;
    char in[IN_SIZE];               /* bytes ready for readers (a ring) */
    uint32_t in_head, in_tail;      /* bytes ever put in / taken out */
    uint16_t lines[MAX_LINES];      /* canonical: lengths of the ready lines */
    uint32_t nlines;
    struct wait_queue readers;
    struct wait_queue writers;      /* pty: a master waiting for room in `in` */
    int fg_pgrp;                    /* foreground process group */
    int hung;                       /* the other end is gone: reads see EOF */
    uint16_t rows, cols;
    tty_out_fn out;                 /* echo goes here */
    void *owner;
};

static const struct termios tio_default = {
    .c_iflag = ICRNL | IXON,
    .c_oflag = OPOST | ONLCR,
    .c_cflag = CFLAG_DEFAULT,
    .c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE | IEXTEN,
    .c_cc = {
        [VINTR] = 3, [VQUIT] = 28, [VERASE] = 127, [VKILL] = 21, [VEOF] = 4,
        [VTIME] = 0, [VMIN] = 1, [VSUSP] = 26, [VWERASE] = 23,
    },
};

static void console_out(void *owner, const char *s, uint32_t n)
{
    (void)owner;
    console_write(s, n);
}

static struct tty console = {
    .tio = tio_default, .rows = 25, .cols = 80, .out = console_out,
};

/* Virtual terminals, as far as programs can tell: Linux has tty1..tty63
   on one screen and switches between them; here every ttyN is the one
   console. An X server still asks for one, makes it "active", and turns
   its keyboard off (K_OFF) — that last part is real: keys then go only to
   /dev/input. */
static int active_vt = 1;
static int kb_mode = K_XLATE;
static struct { int8_t mode, waitv; int16_t relsig, acqsig, frsig; } vt_mode;

static void echo(struct tty *t, const char *s, uint32_t n)
{
    if (t->tio.c_lflag & ECHO)
        t->out(t->owner, s, n);
}

/* Hand bytes to readers. */
static int push(struct tty *t, const char *s, uint32_t n)
{
    if (IN_SIZE - (t->in_head - t->in_tail) < n)
        return -1;                  /* full: drop it */
    for (uint32_t i = 0; i < n; i++)
        t->in[t->in_head++ % IN_SIZE] = s[i];
    return 0;
}

static void wake_readers(struct tty *t)
{
    wq_wake_all(&t->readers);
    wq_wake_all(&poll_wait);            /* poll()/select() may be waiting too */
}

/* Canonical mode: the line is done (Enter, or Ctrl-D). */
static void line_done(struct tty *t)
{
    if (t->nlines < MAX_LINES && push(t, t->line, t->line_len) == 0)
        t->lines[t->nlines++] = t->line_len;
    t->line_len = 0;
    wake_readers(t);
}

static void erase_chars(struct tty *t, uint32_t n)
{
    while (n-- && t->line_len) {
        char c = t->line[--t->line_len];
        /* Control characters were echoed as ^X: two columns to erase. */
        int width = ((t->tio.c_lflag & ECHOCTL) && (uint8_t)c < 32 && c != '\t') ? 2 : 1;
        for (int i = 0; i < width; i++)
            echo(t, "\b \b", 3);
    }
}

static void input_char(struct tty *t, char c)
{
    struct termios *tio = &t->tio;
    uint8_t *cc = tio->c_cc;

    if (c == '\r' && (tio->c_iflag & IGNCR))
        return;
    if (c == '\r' && (tio->c_iflag & ICRNL))
        c = '\n';
    else if (c == '\n' && (tio->c_iflag & INLCR))
        c = '\r';

    if ((tio->c_lflag & ISIG) && (c == cc[VINTR] || c == cc[VQUIT] || c == cc[VSUSP])) {
        int sig = c == cc[VINTR] ? SIGINT : c == cc[VQUIT] ? SIGQUIT : SIGTSTP;
        t->line_len = 0;            /* the half-typed line is thrown away */
        char e[3] = { '^', (char)(c + '@'), '\n' };
        echo(t, e, 3);
        if (t->fg_pgrp)
            signal_send_group(t->fg_pgrp, sig);
        return;
    }

    if (!(tio->c_lflag & ICANON)) { /* raw: straight to the reader */
        if (push(t, &c, 1) == 0)
            wake_readers(t);
        echo(t, &c, 1);
        return;
    }

    if (c == (char)cc[VERASE] || c == '\b') {
        erase_chars(t, 1);
    } else if (c == (char)cc[VKILL]) {
        erase_chars(t, t->line_len);
    } else if (c == (char)cc[VWERASE]) {
        while (t->line_len && t->line[t->line_len - 1] == ' ')
            erase_chars(t, 1);
        while (t->line_len && t->line[t->line_len - 1] != ' ')
            erase_chars(t, 1);
    } else if (c == (char)cc[VEOF]) {
        line_done(t);               /* an empty line here = end of file */
    } else if (c == '\n' || (cc[VEOL] && c == (char)cc[VEOL])) {
        if (t->line_len < LINE_MAX)
            t->line[t->line_len++] = c;
        if ((tio->c_lflag & ECHO) || (tio->c_lflag & ECHONL))
            t->out(t->owner, "\n", 1);
        line_done(t);
    } else if (t->line_len < LINE_MAX - 1) {
        t->line[t->line_len++] = c;
        if ((tio->c_lflag & ECHOCTL) && (uint8_t)c < 32 && c != '\t') {
            char e[2] = { '^', (char)(c + '@') };
            echo(t, e, 2);
        } else {
            echo(t, &c, 1);
        }
    }
}

void tty_feed(struct tty *t, const char *bytes, uint32_t n)
{
    uint32_t flags = irq_save();
    for (uint32_t i = 0; i < n; i++)
        input_char(t, bytes[i]);
    irq_restore(flags);
}

void tty_input(const char *bytes, int n)
{
    tty_feed(&console, bytes, (uint32_t)n);
}

void tty_attach_keyboard(void)
{
    keyboard_set_listener(tty_input);
    vt_set_reply(tty_input);            /* the screen answers via the input */
}

/* A new size (the console's from vt.c, a pty's from TIOCSWINSZ): a
   full-screen program redraws itself when it gets SIGWINCH, as in a
   terminal window being resized. */
static void set_winsize(struct tty *t, uint16_t r, uint16_t c)
{
    if (r == t->rows && c == t->cols)
        return;
    t->rows = r;
    t->cols = c;
    if (t->fg_pgrp)
        signal_send_group(t->fg_pgrp, SIGWINCH);
}

void tty_set_winsize(int r, int c)
{
    set_winsize(&console, (uint16_t)r, (uint16_t)c);
}

int tty_reader_waiting(void)
{
    return console.readers.first != NULL;
}

int tty_foreground(void)
{
    return console.fg_pgrp;
}

void tty_set_foreground(int pgid)
{
    console.fg_pgrp = pgid;
}

/* Can a read() be answered now? */
int tty_readable(struct tty *t)
{
    if (t->hung)
        return 1;                   /* with what's left, then end of file */
    if (t->tio.c_lflag & ICANON)
        return t->nlines > 0;
    uint32_t avail = t->in_head - t->in_tail;
    return t->tio.c_cc[VMIN] ? avail >= 1 : 1;  /* VMIN 0: never wait */
}

uint32_t tty_poll(struct file *f)
{
    (void)f;
    return (tty_readable(&console) ? 0x001 : 0) | 0x004;    /* POLLIN?, always POLLOUT */
}

int32_t tty_read_from(struct tty *t, struct file *f, void *user_buf, uint32_t n)
{
    while (!tty_readable(t)) {
        if (f->flags & O_NONBLOCK)
            return -EAGAIN;
        if (signal_pending())
            return -EINTR;
#ifdef BENCH
        bench_input_wait();
#endif
        wq_sleep(&t->readers);
    }
    uint32_t avail = t->in_head - t->in_tail;
    if (t->tio.c_lflag & ICANON) {
        if (!t->nlines)
            return 0;               /* hung up, nothing left: end of file */
        avail = t->lines[0];        /* at most one line per read() */
        if (avail == 0) {           /* Ctrl-D on an empty line: EOF */
            memmove(t->lines, t->lines + 1, --t->nlines * sizeof t->lines[0]);
            return 0;
        }
    }
    if (avail > IN_SIZE)
        avail = IN_SIZE;            /* can't be, but then n below is bounded */
    if (n > avail)
        n = avail;
    uint32_t at = t->in_tail % IN_SIZE;
    /* Take the bytes before copying them, because the copy can sleep. Every
       reader of the terminal shares this one ring, so a second one would
       otherwise measure the same in_tail, hand out the same bytes, and add n
       again — leaving in_head - in_tail negative, which as an unsigned
       difference stops clamping n, so the next read copies hundreds of
       kilobytes from past the end of `in` straight to user space. lines[0]
       would underflow the same way and `--nlines` at zero turns the memmove
       into a four-billion-byte one. fs/pipe.c had this shape too.
       Nothing is rolled back if the copy fails: the line bookkeeping below
       cannot be undone, and losing a line of typing to a bad pointer is a
       better outcome than an inconsistent ring.
       Once taken, their place in the ring is free and new typing (an
       interrupt, or a pty master) may land there during the copy: so the
       bytes go out of a snapshot. A short read is allowed. */
    char snap[256];
    if (n > sizeof snap)
        n = sizeof snap;
    uint32_t first = n < IN_SIZE - at ? n : IN_SIZE - at;
    memcpy(snap, t->in + at, first);
    memcpy(snap + first, t->in, n - first);
    t->in_tail += n;
    if (t->tio.c_lflag & ICANON) {
        t->lines[0] -= n;
        if (t->lines[0] == 0)
            memmove(t->lines, t->lines + 1, --t->nlines * sizeof t->lines[0]);
    }
    wq_wake_all(&t->writers);
    if (copy_to_user(user_buf, snap, n) < 0)
        return -EFAULT;
    return n;
}

int32_t tty_read(struct file *f, void *user_buf, uint32_t n)
{
    return tty_read_from(&console, f, user_buf, n);
}

static void flush_input(struct tty *t)
{
    t->in_tail = t->in_head;
    t->nlines = 0;
    t->line_len = 0;
    wq_wake_all(&t->writers);
}

void tty_hangup_on(struct tty *t)
{
    flush_input(t);
    if (t->fg_pgrp) {
        signal_send_group(t->fg_pgrp, SIGHUP);
        signal_send_group(t->fg_pgrp, SIGCONT);    /* a stopped job hears it too */
    }
}

void tty_hangup(void)
{
    tty_hangup_on(&console);
}

/* --- other terminals (pty.c) ------------------------------------------------- */

struct tty *tty_new(tty_out_fn out, void *owner)
{
    struct tty *t = kmalloc(sizeof *t);
    if (!t)
        return NULL;
    memset(t, 0, sizeof *t);
    t->tio = tio_default;
    t->rows = 24;
    t->cols = 80;
    t->out = out;
    t->owner = owner;
    return t;
}

void tty_free(struct tty *t)
{
    kfree(t);
}

/* The other end went away: readers get what's left, then end of file. */
void tty_set_hung(struct tty *t)
{
    t->hung = 1;
    wake_readers(t);
    tty_hangup_on(t);
}

/* Room for typing: what the master may write without anything being
   dropped. A whole line's worth is kept back, so a line finished in
   canonical mode always fits. */
uint32_t tty_space(struct tty *t)
{
    uint32_t free = IN_SIZE - (t->in_head - t->in_tail);
    return free > LINE_MAX ? free - LINE_MAX : 0;
}

void tty_wait_space(struct tty *t)
{
    wq_sleep(&t->writers);
}

/* Output processing: should '\n' go out as "\r\n"? */
int tty_onlcr(struct tty *t)
{
    return (t->tio.c_oflag & (OPOST | ONLCR)) == (OPOST | ONLCR);
}

int tty_get_fg(struct tty *t) { return t->fg_pgrp; }
void tty_set_fg(struct tty *t, int pgid) { t->fg_pgrp = pgid; }

/*
 * May this process reconfigure the console?
 *
 * Linux asks for CAP_SYS_TTY_CONFIG; there are no capabilities here yet
 * (docs/gaps.md), so it is root. The calls it guards are the ones that take
 * the console away from everybody else — turning the keyboard off, switching
 * to graphics, changing VT — and an X server is started by root.
 */
static int tty_config_allowed(void)
{
    return proc_current()->cred.euid == 0;
}

/* The ioctls every terminal has; -ENOIOCTLCMD for the rest. */
#define ENOIOCTLCMD 515
static int32_t common_ioctl(struct tty *t, uint32_t cmd, uintptr_t arg)
{
    switch (cmd) {
    case TCGETS:
        return copy_to_user((void *)arg, &t->tio, sizeof t->tio);
    case TCSETS:
    case TCSETSW:
    case TCSETSF: {
        struct termios n;
        if (copy_from_user(&n, (void *)arg, sizeof n) < 0)
            return -EFAULT;
        uint32_t flags = irq_save();
        if (cmd == TCSETSF)
            flush_input(t);
        if (!(t->tio.c_lflag & ICANON) && (n.c_lflag & ICANON) &&
            t->in_head != t->in_tail && t->nlines < MAX_LINES)
            t->lines[t->nlines++] = t->in_head - t->in_tail;   /* raw bytes: one "line" */
        if ((t->tio.c_lflag & ICANON) && !(n.c_lflag & ICANON)) {
            t->nlines = 0;          /* raw: just bytes now */
            if (t->line_len && push(t, t->line, t->line_len) == 0)
                t->line_len = 0;    /* keys typed ahead stay readable */
        }
        t->tio = n;
        irq_restore(flags);
        wake_readers(t);            /* raw now: what's there may be enough */
        return 0;
    }
    case TIOCGWINSZ: {
        uint16_t ws[4] = { t->rows, t->cols, 0, 0 };
        return copy_to_user((void *)arg, ws, sizeof ws);
    }
    case TIOCSWINSZ: {
        uint16_t ws[4];
        if (copy_from_user(ws, (void *)arg, sizeof ws) < 0)
            return -EFAULT;
        set_winsize(t, ws[0], ws[1]);
        return 0;
    }
    case TIOCGPGRP: {
        int pg = t->fg_pgrp ? t->fg_pgrp : proc_current()->pgid;
        return copy_to_user((void *)arg, &pg, sizeof pg);
    }
    case TIOCSPGRP: {
        int pg;
        if (copy_from_user(&pg, (void *)arg, sizeof pg) < 0)
            return -EFAULT;
        /* The foreground group is who gets the keyboard, and SIGINT. Without
           a check here any process could point the terminal at itself and
           read what is typed at the shell, or at a group that does not exist
           and leave the keyboard going nowhere. Linux's rule: the group has
           to be live and in the caller's session. */
        if (pg <= 0)
            return -EINVAL;
        if (!proc_pgid_in_session(pg, proc_current()->sid))
            return -EPERM;
        t->fg_pgrp = pg;
        return 0;
    }
    case FIONREAD: {
        int n = (int)(t->in_head - t->in_tail);
        return copy_to_user((void *)arg, &n, sizeof n);
    }
    case TCFLSH:
        flush_input(t);
        return 0;
    default:
        return -ENOIOCTLCMD;
    }
}

int32_t tty_ioctl_on(struct tty *t, uint32_t cmd, uintptr_t arg)
{
    int32_t r = common_ioctl(t, cmd, arg);
    return r == -ENOIOCTLCMD ? -EINVAL : r;
}

int32_t tty_ioctl(struct file *f, uint32_t cmd, uintptr_t arg)
{
    (void)f;
    int32_t r = common_ioctl(&console, cmd, arg);
    if (r != -ENOIOCTLCMD)
        return r;
    switch (cmd) {
    case TIOCSCTTY:
    case TIOCNOTTY:
        return 0;
    /* A program that draws on the screen itself (an X server, a game on
       /dev/fb0) switches the console off with KD_GRAPHICS and back on with
       KD_TEXT, which redraws it. */
    case KDSETMODE:
        if (!tty_config_allowed())
            return -EPERM;
        if (arg != KD_TEXT && arg != KD_GRAPHICS)
            return -EINVAL;
        vt_set_graphics(arg == KD_GRAPHICS);
        return 0;
    case KDGETMODE: {
        int mode = vt_graphics() ? KD_GRAPHICS : KD_TEXT;
        return copy_to_user((void *)arg, &mode, sizeof mode);
    }
    case KDGKBTYPE: {
        char type = KB_101;
        return copy_to_user((void *)arg, &type, 1);
    }
    case KDGKBMODE:
        return copy_to_user((void *)arg, &kb_mode, sizeof kb_mode);
    case KDSKBMODE:
        /* K_OFF stops keys reaching the console at all. Getting them back
           means issuing this ioctl again, which means typing — so an
           unprivileged process that sets it has locked the machine's console
           with no way back. The most expensive thing in this switch to leave
           open, and the cheapest to close. */
        if (!tty_config_allowed())
            return -EPERM;
        if (arg > K_OFF)
            return -EINVAL;
        kb_mode = (int)arg;
        keyboard_set_tty(arg == K_XLATE || arg == K_UNICODE);
        return 0;
    case VT_OPENQRY: {
        int free_vt = 7;            /* where X servers usually go */
        return copy_to_user((void *)arg, &free_vt, sizeof free_vt);
    }
    case VT_GETSTATE: {
        /* v_active, v_signal, then the mask of VTs in use. The shift is
           bounded because VT_ACTIVATE accepts up to 63 and `1 << 32` or more
           on a 32-bit int is undefined — the compiler may do anything with
           it, and on x86 the hardware shifts by the count modulo 32, so 32
           would have set bit 0. Nothing is lost by stopping at 16: the field
           is 16 bits wide. */
        uint16_t mask = 1 << 1;
        if (active_vt < 16)
            mask |= (uint16_t)(1u << active_vt);
        uint16_t st[3] = { (uint16_t)active_vt, 0, mask };
        return copy_to_user((void *)arg, st, sizeof st);
    }
    case VT_ACTIVATE:
        if (!tty_config_allowed())
            return -EPERM;
        if (arg < 1 || arg > 63)
            return -ENXIO;
        active_vt = (int)arg;
        return 0;
    case VT_WAITACTIVE:
    case VT_RELDISP:
    case VT_DISALLOCATE:
        return 0;
    case VT_GETMODE:
        return copy_to_user((void *)arg, &vt_mode, sizeof vt_mode);
    case VT_SETMODE: {
        /* Into a local and then assigned, not straight into the global: the
           copy can sleep, so copying into vt_mode leaves it half old and
           half new for as long as the fault takes, and a copy that fails
           leaves it that way for good. */
        __typeof__(vt_mode) m;
        if (!tty_config_allowed())
            return -EPERM;
        if (copy_from_user(&m, (void *)arg, sizeof m) < 0)
            return -EFAULT;
        vt_mode = m;
        return 0;
    }
    default:
        return -EINVAL;
    }
}
