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
 */
#include <errno.h>
#include <interrupts.h>
#include <keyboard.h>
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

#define LINE_MAX  256               /* the line being edited */
#define IN_SIZE   2048              /* typed bytes waiting for a reader */
#define MAX_LINES 32                /* complete lines waiting */

static struct termios tio = {
    .c_iflag = ICRNL | IXON,
    .c_oflag = OPOST | ONLCR,
    .c_cflag = CFLAG_DEFAULT,
    .c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE | IEXTEN,
    .c_cc = {
        [VINTR] = 3, [VQUIT] = 28, [VERASE] = 127, [VKILL] = 21, [VEOF] = 4,
        [VTIME] = 0, [VMIN] = 1, [VSUSP] = 26, [VWERASE] = 23,
    },
};

static char line[LINE_MAX];         /* canonical mode: the line being typed */
static uint32_t line_len;

static char in[IN_SIZE];            /* bytes ready for readers (a ring) */
static uint32_t in_head, in_tail;   /* bytes ever put in / taken out */
static uint16_t lines[MAX_LINES];   /* canonical: lengths of the ready lines */
static uint32_t nlines;

static struct wait_queue readers;
static int fg_pgrp;                 /* foreground process group */
static uint16_t rows = 25, cols = 80;

static void echo(const char *s, uint32_t n)
{
    if (tio.c_lflag & ECHO)
        console_write(s, n);
}

/* Hand bytes to readers. */
static int push(const char *s, uint32_t n)
{
    if (IN_SIZE - (in_head - in_tail) < n)
        return -1;                  /* full: drop it */
    for (uint32_t i = 0; i < n; i++)
        in[in_head++ % IN_SIZE] = s[i];
    return 0;
}

/* Canonical mode: the line is done (Enter, or Ctrl-D). */
static void wake_readers(void)
{
    wq_wake_all(&readers);
    wq_wake_all(&poll_wait);            /* poll()/select() may be waiting too */
}

static void line_done(void)
{
    if (nlines < MAX_LINES && push(line, line_len) == 0)
        lines[nlines++] = line_len;
    line_len = 0;
    wake_readers();
}

static void erase_chars(uint32_t n)
{
    while (n-- && line_len) {
        char c = line[--line_len];
        /* Control characters were echoed as ^X: two columns to erase. */
        int width = ((tio.c_lflag & ECHOCTL) && (uint8_t)c < 32 && c != '\t') ? 2 : 1;
        for (int i = 0; i < width; i++)
            echo("\b \b", 3);
    }
}

static void input_char(char c)
{
    uint8_t *cc = tio.c_cc;

    if (c == '\r' && (tio.c_iflag & IGNCR))
        return;
    if (c == '\r' && (tio.c_iflag & ICRNL))
        c = '\n';
    else if (c == '\n' && (tio.c_iflag & INLCR))
        c = '\r';

    if ((tio.c_lflag & ISIG) && (c == cc[VINTR] || c == cc[VQUIT] || c == cc[VSUSP])) {
        int sig = c == cc[VINTR] ? SIGINT : c == cc[VQUIT] ? SIGQUIT : SIGTSTP;
        line_len = 0;               /* the half-typed line is thrown away */
        char e[3] = { '^', (char)(c + '@'), '\n' };
        echo(e, 3);
        if (fg_pgrp)
            signal_send_group(fg_pgrp, sig);
        return;
    }

    if (!(tio.c_lflag & ICANON)) {  /* raw: straight to the reader */
        if (push(&c, 1) == 0)
            wake_readers();
        echo(&c, 1);
        return;
    }

    if (c == (char)cc[VERASE] || c == '\b') {
        erase_chars(1);
    } else if (c == (char)cc[VKILL]) {
        erase_chars(line_len);
    } else if (c == (char)cc[VWERASE]) {
        while (line_len && line[line_len - 1] == ' ')
            erase_chars(1);
        while (line_len && line[line_len - 1] != ' ')
            erase_chars(1);
    } else if (c == (char)cc[VEOF]) {
        line_done();                /* an empty line here = end of file */
    } else if (c == '\n' || (cc[VEOL] && c == (char)cc[VEOL])) {
        if (line_len < LINE_MAX)
            line[line_len++] = c;
        if ((tio.c_lflag & ECHO) || (tio.c_lflag & ECHONL))
            console_write("\n", 1);
        line_done();
    } else if (line_len < LINE_MAX - 1) {
        line[line_len++] = c;
        if ((tio.c_lflag & ECHOCTL) && (uint8_t)c < 32 && c != '\t') {
            char e[2] = { '^', (char)(c + '@') };
            echo(e, 2);
        } else {
            echo(&c, 1);
        }
    }
}

void tty_input(const char *bytes, int n)
{
    uint32_t flags = irq_save();
    for (int i = 0; i < n; i++)
        input_char(bytes[i]);
    irq_restore(flags);
}

void tty_attach_keyboard(void)
{
    keyboard_set_listener(tty_input);
    vt_set_reply(tty_input);            /* the screen answers via the input */
}

/* The console changed size (vt.c): a full-screen program redraws itself
   when it gets SIGWINCH, as in a terminal window being resized. */
void tty_set_winsize(int r, int c)
{
    if (r == rows && c == cols)
        return;
    rows = (uint16_t)r;
    cols = (uint16_t)c;
    if (fg_pgrp)
        signal_send_group(fg_pgrp, SIGWINCH);
}

int tty_reader_waiting(void)
{
    return readers.first != NULL;
}

int tty_foreground(void)
{
    return fg_pgrp;
}

void tty_set_foreground(int pgid)
{
    fg_pgrp = pgid;
}

/* Can a read() be answered now? */
static int readable(void)
{
    if (tio.c_lflag & ICANON)
        return nlines > 0;
    uint32_t avail = in_head - in_tail;
    return tio.c_cc[VMIN] ? avail >= 1 : 1;     /* VMIN 0: never wait */
}

uint32_t tty_poll(struct file *f)
{
    (void)f;
    return (readable() ? 0x001 : 0) | 0x004;    /* POLLIN?, always POLLOUT */
}

int32_t tty_read(struct file *f, void *user_buf, uint32_t n)
{
    while (!readable()) {
        if (f->flags & O_NONBLOCK)
            return -EAGAIN;
        if (signal_pending())
            return -EINTR;
#ifdef BENCH
        bench_input_wait();
#endif
        wq_sleep(&readers);
    }
    uint32_t avail = in_head - in_tail;
    if (tio.c_lflag & ICANON) {
        avail = lines[0];           /* at most one line per read() */
        if (avail == 0) {           /* Ctrl-D on an empty line: EOF */
            memmove(lines, lines + 1, --nlines * sizeof lines[0]);
            return 0;
        }
    }
    if (avail > IN_SIZE)
        avail = IN_SIZE;            /* can't be, but then n below is bounded */
    if (n > avail)
        n = avail;
    uint32_t at = in_tail % IN_SIZE;
    uint32_t first = n < IN_SIZE - at ? n : IN_SIZE - at;
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
       better outcome than an inconsistent ring. */
    in_tail += n;
    if (tio.c_lflag & ICANON) {
        lines[0] -= n;
        if (lines[0] == 0)
            memmove(lines, lines + 1, --nlines * sizeof lines[0]);
    }
    if (copy_to_user(user_buf, in + at, first) < 0 ||
        copy_to_user((char *)user_buf + first, in, n - first) < 0)
        return -EFAULT;
    return n;
}

static void flush_input(void)
{
    in_tail = in_head;
    nlines = 0;
    line_len = 0;
}

void tty_hangup(void)
{
    flush_input();
    if (fg_pgrp) {
        signal_send_group(fg_pgrp, SIGHUP);
        signal_send_group(fg_pgrp, SIGCONT);    /* a stopped job hears it too */
    }
}

int32_t tty_ioctl(struct file *f, uint32_t cmd, uint32_t arg)
{
    (void)f;
    switch (cmd) {
    case TCGETS:
        return copy_to_user((void *)arg, &tio, sizeof tio);
    case TCSETS:
    case TCSETSW:
    case TCSETSF: {
        struct termios t;
        if (copy_from_user(&t, (void *)arg, sizeof t) < 0)
            return -EFAULT;
        uint32_t flags = irq_save();
        if (cmd == TCSETSF)
            flush_input();
        if (!(tio.c_lflag & ICANON) && (t.c_lflag & ICANON) &&
            in_head != in_tail && nlines < MAX_LINES)
            lines[nlines++] = in_head - in_tail;   /* raw bytes: one "line" */
        if ((tio.c_lflag & ICANON) && !(t.c_lflag & ICANON)) {
            nlines = 0;             /* raw: just bytes now */
            if (line_len && push(line, line_len) == 0)
                line_len = 0;       /* keys typed ahead stay readable */
        }
        tio = t;
        irq_restore(flags);
        return 0;
    }
    case TIOCGWINSZ: {
        uint16_t ws[4] = { rows, cols, 0, 0 };
        return copy_to_user((void *)arg, ws, sizeof ws);
    }
    case TIOCSWINSZ: {
        uint16_t ws[4];
        if (copy_from_user(ws, (void *)arg, sizeof ws) < 0)
            return -EFAULT;
        rows = ws[0];
        cols = ws[1];
        return 0;
    }
    case TIOCGPGRP: {
        int pg = fg_pgrp ? fg_pgrp : proc_current()->pgid;
        return copy_to_user((void *)arg, &pg, sizeof pg);
    }
    case TIOCSPGRP: {
        int pg;
        if (copy_from_user(&pg, (void *)arg, sizeof pg) < 0)
            return -EFAULT;
        fg_pgrp = pg;
        return 0;
    }
    case FIONREAD: {
        int n = (int)(in_head - in_tail);
        return copy_to_user((void *)arg, &n, sizeof n);
    }
    case TCFLSH:
        flush_input();
        return 0;
    case TIOCSCTTY:
    case TIOCNOTTY:
        return 0;
    /* A program that draws on the screen itself (an X server, a game on
       /dev/fb0) switches the console off with KD_GRAPHICS and back on with
       KD_TEXT, which redraws it. */
    case KDSETMODE:
        if (arg != KD_TEXT && arg != KD_GRAPHICS)
            return -EINVAL;
        vt_set_graphics(arg == KD_GRAPHICS);
        return 0;
    case KDGETMODE: {
        int mode = vt_graphics() ? KD_GRAPHICS : KD_TEXT;
        return copy_to_user((void *)arg, &mode, sizeof mode);
    }
    default:
        return -EINVAL;
    }
}
