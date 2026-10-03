/*
 * ttytest.c — step 8 tests: the terminal
 *
 * The kernel test "types" at this program (tty_input) whenever it waits in
 * read(): see kernel/user_test.c for the keys.
 *
 *   1. canonical mode: "hello wq<Backspace>orld<Enter>" arrives as one line,
 *      "hello world\n" — the terminal did the editing
 *   2. Ctrl-D on an empty line: read() returns 0 (end of file)
 *   3. raw mode (no ICANON, no ECHO): the up-arrow arrives at once as its
 *      3 bytes, ESC [ A
 *   4. Ctrl-C: SIGINT for the foreground group; read() returns -EINTR
 *   + window size, and isatty() is false for a pipe
 */
#include <mini.h>

static int checks;

#define CHECK(cond)                                         \
    do {                                                    \
        checks++;                                           \
        if (!(cond)) {                                      \
            put("ttytest: check ");                         \
            put_u64(checks);                                \
            put(" failed: " #cond "\n");                    \
            return checks;                                  \
        }                                                   \
    } while (0)

static volatile int interrupted;
static void on_int(int sig) { (void)sig; interrupted = 1; }

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    char buf[64];
    long n;

    /* 1. A whole, edited line. */
    n = read(0, buf, sizeof buf);
    CHECK(n == 12 && memcmp(buf, "hello world\n", 12) == 0);

    /* 2. Ctrl-D: end of file. */
    CHECK(read(0, buf, sizeof buf) == 0);

    /* 3. Raw mode: keys as they come. */
    struct termios t, saved;
    CHECK(ioctl(0, TCGETS, &saved) == 0 && (saved.c_lflag & ICANON));
    t = saved;
    t.c_lflag &= ~(ICANON | ECHO);
    t.c_cc[VMIN] = 1;
    CHECK(ioctl(0, TCSETS, &t) == 0);
    n = read(0, buf, sizeof buf);
    CHECK(n == 3 && buf[0] == 27 && buf[1] == '[' && buf[2] == 'A');
    CHECK(ioctl(0, TCSETS, &saved) == 0);

    /* 4. Ctrl-C: we're the foreground group, so SIGINT comes to us. */
    CHECK(sigaction_simple(SIGINT, on_int, 0, 0) == 0);
    int pg = getpgrp();
    CHECK(pg == getpid());              /* started by the kernel: own group */
    CHECK(ioctl(0, TIOCSPGRP, &pg) == 0);
    n = read(0, buf, sizeof buf);
    CHECK(n == -EINTR && interrupted);

    /* Window size (80x25 on the text screen, more on a framebuffer);
       and a pipe is not a terminal (isatty() = 0). */
    unsigned short ws[4];
    CHECK(ioctl(1, TIOCGWINSZ, ws) == 0 && ws[0] >= 25 && ws[1] >= 80);
    int p[2];
    CHECK(pipe(p) == 0 && ioctl(p[0], TCGETS, &t) == -ENOTTY);

    put("ttytest: all ");
    put_u64(checks);
    put(" checks passed\n");
    return 0;
}
