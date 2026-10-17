/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * xshmbench — how fast a picture reaches the X server: down the socket
 * (XPutImage), and through shared memory (XShmPutImage, the MIT-SHM
 * extension). Every GTK, cairo and Imlib2 program sends its pictures one of
 * these two ways, so this is the cost of every icon, photo and web page.
 *
 *   xshmbench [width height [count]]      (default: 1024 768 50)
 *
 * Each way is timed twice: into a window on the screen (what a program
 * sees, including the X server drawing into video memory), and into a
 * pixmap in the server's RAM (only the transfer, which is what MIT-SHM
 * changes). One picture of each goes first, untimed: the first one pays
 * for the server's own page faults.
 *
 * A measuring tool, not part of the live disk: build it against the live
 * sysroot and copy it in (docs/performance.md says how it was used).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <time.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static int x_failed;
static double t_start;
static int on_error(Display *d, XErrorEvent *e)
{
    (void)d;
    x_failed = e->error_code;
    return 0;
}

static void report(const char *how, const char *to, int w, int h, double per)
{
    printf("%-13s %-7s %dx%d: %7.2f ms a picture, %6.0f MB/s\n", how, to, w, h, per * 1e3,
           (double)w * h * 4 / per / 1e6);
}

/* n pictures to `to`, one way or the other (simg: shared memory); the time
   for one, after an untimed first. */
static double timed(Display *d, Drawable to, GC gc, XImage *img, XImage *simg, int w, int h, int n)
{
    for (int i = -1; i < n; i++) {
        if (i == 0) {
            XSync(d, False);
            t_start = now();
        }
        if (simg)
            XShmPutImage(d, to, gc, simg, 0, 0, 0, 0, w, h, False);
        else
            XPutImage(d, to, gc, img, 0, 0, 0, 0, w, h);
    }
    XSync(d, False);
    return (now() - t_start) / n;
}

int main(int argc, char **argv)
{
    int w = argc > 2 ? atoi(argv[1]) : 1024, h = argc > 2 ? atoi(argv[2]) : 768;
    int n = argc > 3 ? atoi(argv[3]) : 50;
    Display *d = XOpenDisplay(NULL);
    if (!d) {
        fprintf(stderr, "xshmbench: no X display\n");
        return 1;
    }
    XSetErrorHandler(on_error);
    int s = DefaultScreen(d), depth = DefaultDepth(d, s);
    Visual *v = DefaultVisual(d, s);
    Window win = XCreateSimpleWindow(d, RootWindow(d, s), 0, 0, w, h, 0, 0, 0);
    XMapWindow(d, win);
    Pixmap pm = XCreatePixmap(d, win, w, h, depth);
    GC gc = XCreateGC(d, win, 0, NULL);
    XSync(d, False);

    /* Down the socket. */
    size_t bytes = (size_t)w * h * 4;
    char *pixels = malloc(bytes);
    if (!pixels)
        return 1;
    for (size_t i = 0; i < bytes; i++)
        pixels[i] = (char)(i * 7);
    XImage *img = XCreateImage(d, v, depth, ZPixmap, 0, pixels, w, h, 32, 0);
    double put_win = timed(d, win, gc, img, NULL, w, h, n);
    double put_pm = timed(d, pm, gc, img, NULL, w, h, n);
    report("XPutImage", "window", w, h, put_win);
    report("XPutImage", "pixmap", w, h, put_pm);

    /* Through shared memory. */
    if (!XShmQueryExtension(d)) {
        printf("MIT-SHM: the X server doesn't offer it\n");
        return 0;
    }
    XShmSegmentInfo si;
    XImage *simg = XShmCreateImage(d, v, depth, ZPixmap, NULL, &si, w, h);
    si.shmid = shmget(IPC_PRIVATE, (size_t)simg->bytes_per_line * simg->height, IPC_CREAT | 0600);
    si.shmaddr = simg->data = si.shmid < 0 ? (char *)-1 : shmat(si.shmid, NULL, 0);
    si.readOnly = False;
    if (si.shmid < 0 || si.shmaddr == (char *)-1) {
        perror("xshmbench: shmget/shmat");
        return 1;
    }
    XShmAttach(d, &si);
    XSync(d, False);
    shmctl(si.shmid, IPC_RMID, NULL);   /* freed when both have let go */
    if (x_failed) {
        printf("MIT-SHM: the X server refused the segment (X error %d)\n", x_failed);
        return 1;
    }
    memcpy(simg->data, pixels, bytes < (size_t)simg->bytes_per_line * simg->height
                                   ? bytes : (size_t)simg->bytes_per_line * simg->height);
    double shm_win = timed(d, win, gc, NULL, simg, w, h, n);
    double shm_pm = timed(d, pm, gc, NULL, simg, w, h, n);
    report("XShmPutImage", "window", w, h, shm_win);
    report("XShmPutImage", "pixmap", w, h, shm_pm);
    printf("shared memory: %.1fx as fast to a window, %.1fx as fast to a pixmap\n",
           put_win / shm_win, put_pm / shm_pm);
    XShmDetach(d, &si);
    shmdt(si.shmaddr);
    XCloseDisplay(d);
    return 0;
}
