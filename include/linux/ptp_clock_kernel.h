#ifndef LINUX_PTP_CLOCK_KERNEL_H
#define LINUX_PTP_CLOCK_KERNEL_H
/* No PTP clocks: registering one fails, which the driver accepts. */
#include <linux/err.h>
#include <linux/ktime.h>
#include <linux/time.h>
struct device;
struct ptp_clock;
struct ptp_clock_request;
struct ptp_system_timestamp;
struct system_device_crosststamp {
    ktime_t device;
    ktime_t sys_realtime;
    ktime_t sys_monoraw;
    ktime_t sys_systime;            /* (ours: what the driver fills) */
    int clock_id;
};
struct module;
struct ptp_clock_info {
    struct module *owner;
    char name[32];
    s32 max_adj;
    int (*adjfine)(struct ptp_clock_info *p, long scaled_ppm);
    int (*adjtime)(struct ptp_clock_info *p, s64 delta);
    int (*gettime64)(struct ptp_clock_info *p, struct timespec64 *ts);
    int (*gettimex64)(struct ptp_clock_info *p, struct timespec64 *ts, struct ptp_system_timestamp *s);
    int (*settime64)(struct ptp_clock_info *p, const struct timespec64 *ts);
    int (*getcrosststamp)(struct ptp_clock_info *p, struct system_device_crosststamp *x);
    int (*enable)(struct ptp_clock_info *p, struct ptp_clock_request *r, int on);
};
static inline struct ptp_clock *ptp_clock_register(struct ptp_clock_info *i, struct device *d)
{ (void)i; (void)d; return ERR_PTR(-EOPNOTSUPP); }
static inline int ptp_clock_unregister(struct ptp_clock *c) { (void)c; return 0; }
static inline int ptp_clock_index(struct ptp_clock *c) { (void)c; return -1; }
static inline long scaled_ppm_to_ppb(long ppm) { return ppm * 125 / 8192; }
static inline s64 adjust_by_scaled_ppm(s64 base, long scaled_ppm) { return base + base * scaled_ppm / (65536LL * 1000000); }
#endif
