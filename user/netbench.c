/*
 * netbench.c — what do UDP and TCP request/response cost the client machine?
 *
 * The same static binary runs on MyOS and on Linux (as Linux's /init), on
 * the same QEMU e1000, so the numbers compare the two kernels' code paths.
 * A UDP echo server on the host answers through QEMU's user network
 * (10.0.2.2:7777 UDP and :7778 TCP in the guest are 127.0.0.1 on the host).
 *
 * Per 64-byte echo we time:
 *   write()   sending it on a connected UDP socket
 *   read()    taking the echo off the socket (it is already there)
 *   kernel    everything else the kernel does for the packet: the receive
 *             interrupt, driver, IP/UDP, waking, transmit clean-up. We spin
 *             on the cycle counter and add up the gaps where the kernel had
 *             the CPU, minus the gaps in an equally long window with no
 *             traffic (timer ticks).
 * Under `-icount shift=0` the "cycles" are guest instructions executed.
 */
#include <mini.h>

#define SYS_mount       21
#define SYS_reboot      88
#define SYS_nanosleep  162
#define SYS_finit_module 350
#define SYS_socket     359
#define SYS_connect    362
#define SYS_setsockopt 366
#define AF_INET 2
#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define IPPROTO_TCP 6
#define TCP_NODELAY 1
#define TCP_ROUNDS 800

#define BATCH   8               /* MyOS queues 8 datagrams per socket */
#define ROUNDS  200
#define WINDOW  3000000ull      /* cycles to spin while the echoes arrive */
#define GAP     150             /* a loop pass longer than this was the kernel */
#define WARMUP  20

struct sockaddr_in { uint16_t family, port; uint32_t addr; uint8_t zero[8]; };

static void msleep(long ms)
{
    long ts[2] = { ms / 1000, (ms % 1000) * 1000000 };
    syscall3(SYS_nanosleep, (long)ts, 0, 0);
}

/* Spin for `len` cycles; return the cycles the kernel took in between. */
static uint64_t spin(uint64_t len)
{
    uint64_t start = rdtsc(), prev = start, stolen = 0;
    for (;;) {
        uint64_t now = rdtsc();
        if (now - prev > GAP)
            stolen += now - prev;
        prev = now;
        if (now - start >= len)
            return stolen;
    }
}

static void line(const char *what, uint64_t v)
{
    put("netbench: ");
    put(what);
    for (size_t n = strlen(what); n < 34; n++)
        put(" ");
    put_u64(v);
    put("\n");
}

/* --- Linux only: be init — load the e1000 driver, give eth0 an address --- */

struct ifreq { char name[16]; union { struct sockaddr_in a; short flags; uint8_t pad[16]; } u; };

static void linux_setup(void)
{
    syscall5(SYS_mount, (long)"devtmpfs", (long)"/dev", (long)"devtmpfs", 0, 0);
    int con = open("/dev/ttyS0", O_RDWR);
    dup2(con, 1);
    dup2(con, 2);
    int ko = open("/e1000.ko", O_RDONLY);
    long r = syscall3(SYS_finit_module, ko, (long)"", 0);
    if (r < 0) {
        put("netbench: loading e1000.ko failed\n");
        return;
    }
    int s = (int)syscall6_int80(SYS_socket, AF_INET, SOCK_DGRAM, 0, 0, 0, 0);
    struct ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    memcpy(ifr.name, "eth0", 5);
    ifr.u.a.family = AF_INET;
    ifr.u.a.addr = 0x0F02000A;                      /* 10.0.2.15 */
    ioctl(s, 0x8916, &ifr);                         /* SIOCSIFADDR */
    ifr.u.a.addr = 0x00FFFFFF;                      /* 255.255.255.0 */
    ioctl(s, 0x891C, &ifr);                         /* SIOCSIFNETMASK */
    memset(&ifr.u, 0, sizeof ifr.u);
    ifr.u.flags = 0x1 | 0x40;                       /* IFF_UP | IFF_RUNNING */
    if (ioctl(s, 0x8914, &ifr) < 0)                 /* SIOCSIFFLAGS */
        put("netbench: eth0 up failed\n");
    close(s);
    msleep(3000);                                   /* link up, ARP-ready */
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    int is_init = getpid() == 1;
    if (is_init)
        linux_setup();

    /* Baseline: an empty system call. */
    for (int i = 0; i < 1000; i++)
        getpid();
    uint64_t t = rdtsc();
    for (int i = 0; i < 20000; i++)
        getpid();
    line("null syscall (getpid)", (rdtsc() - t) / 20000);

    int s = (int)syscall6_int80(SYS_socket, AF_INET, SOCK_DGRAM, 0, 0, 0, 0);
    struct sockaddr_in to = { AF_INET, 0x611E, 0x0202000A, {0} };   /* 10.0.2.2:7777 */
    if (s < 0 || syscall6_int80(SYS_connect, s, (long)&to, sizeof to, 0, 0, 0) < 0) {
        put("netbench: no UDP socket\n");
        goto out;
    }
    fcntl(s, F_SETFL, O_NONBLOCK);

    uint8_t buf[64], in[256];
    memset(buf, 'x', sizeof buf);
    uint64_t wr = 0, rd = 0, busy = 0, idle = 0;
    uint32_t sent = 0, got = 0, lost = 0;
    for (int round = 0; round < WARMUP + ROUNDS; round++) {
        int timed = round >= WARMUP;
        uint64_t w = 0, r = 0;
        uint32_t n = 0;
        for (int i = 0; i < BATCH; i++) {
            t = rdtsc();
            long k = write(s, buf, sizeof buf);
            w += rdtsc() - t;
            if (k != (long)sizeof buf)
                put("netbench: write failed\n");
        }
        uint64_t b = spin(WINDOW);
        for (int i = 0; i < BATCH; i++) {
            t = rdtsc();
            long k = read(s, in, sizeof in);
            uint64_t c = rdtsc() - t;
            if (k == (long)sizeof buf) {
                r += c;
                n++;
            }
        }
        while (read(s, in, sizeof in) > 0)         /* late echoes */
            ;
        uint64_t i0 = spin(WINDOW);
        if (!timed)
            continue;
        if (n != BATCH) {                          /* only whole rounds count */
            lost += BATCH - n;
            continue;
        }
        wr += w;
        rd += r;
        busy += b;
        idle += i0;
        sent += BATCH;
        got += n;
    }
    if (!got) {
        put("netbench: no echoes came back (is tools/echo-server.py running?)\n");
        goto out;
    }
    line("udp packets timed", got);
    line("udp lost (rounds skipped)", lost);
    line("udp write() 64 B", wr / sent);
    line("udp read() 64 B", rd / got);
    line("udp kernel work outside syscalls", busy > idle ? (busy - idle) / got : 0);
    line("udp total per echo", (wr + rd + (busy > idle ? busy - idle : 0)) / got);
    close(s);

    /* TCP: one 64-byte write, wait, read the 64 bytes back. TCP_NODELAY on
       both kernels (MyOS never delays a small send; Linux would, by Nagle). */
    s = (int)syscall6_int80(SYS_socket, AF_INET, SOCK_STREAM, 0, 0, 0, 0);
    struct sockaddr_in tto = { AF_INET, 0x621E, 0x0202000A, {0} };  /* 10.0.2.2:7778 */
    if (s < 0 || syscall6_int80(SYS_connect, s, (long)&tto, sizeof tto, 0, 0, 0) < 0) {
        put("netbench: no TCP connection (is tools/echo-server.py running?)\n");
        goto out;
    }
    int one = 1;
    syscall6_int80(SYS_setsockopt, s, IPPROTO_TCP, TCP_NODELAY, (long)&one, 4, 0);
    fcntl(s, F_SETFL, O_NONBLOCK);
    wr = rd = busy = idle = 0;
    got = lost = 0;
    for (int round = 0; round < WARMUP + TCP_ROUNDS; round++) {
        t = rdtsc();
        long k = write(s, buf, sizeof buf);
        uint64_t w = rdtsc() - t;
        if (k != (long)sizeof buf) {
            put("netbench: tcp write failed, round ");
            put_u64((uint64_t)round);
            put(k < 0 ? " error -" : " returned ");
            put_u64((uint64_t)(k < 0 ? -k : k));
            put("\n");
            break;
        }
        uint64_t b = spin(WINDOW);
        uint64_t r = 0;
        long have = 0;
        for (int tries = 0; have < (long)sizeof buf && tries < 4; tries++) {
            t = rdtsc();
            k = read(s, in + have, sizeof in - have);
            r += rdtsc() - t;
            if (k > 0)
                have += k;
            else if (have < (long)sizeof buf)
                b += spin(WINDOW);              /* late: wait some more */
        }
        uint64_t i0 = spin(WINDOW);
        if (round < WARMUP)
            continue;
        if (have != (long)sizeof buf) {
            lost++;
            continue;
        }
        wr += w;
        rd += r;
        busy += b;
        idle += i0;
        got++;
    }
    if (!got) {
        put("netbench: no TCP echoes came back\n");
        goto out;
    }
    line("tcp echoes timed", got);
    line("tcp late (rounds skipped)", lost);
    line("tcp write() 64 B", wr / got);
    line("tcp read() 64 B", rd / got);
    line("tcp kernel work outside syscalls", busy > idle ? (busy - idle) / got : 0);
    line("tcp total per echo", (wr + rd + (busy > idle ? busy - idle : 0)) / got);
out:
    put("netbench: done\n");
    if (is_init)
        syscall3(SYS_reboot, 0xfee1dead, 672274793, 0x4321fedc);    /* power off */
    return 0;
}
