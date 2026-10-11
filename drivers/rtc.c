/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * rtc.c — the battery-backed clock (CMOS RTC): what time is it?
 *
 * Every PC has a small clock chip that keeps the date and time while the
 * power is off. It's read through two I/O ports: 0x70 selects a register,
 * 0x71 reads it. Values are usually in BCD (0x59 means 59). We read it
 * once at boot; after that the 1 ms timer keeps time.
 */
#include <io.h>
#include <rtc.h>

static uint8_t cmos(uint8_t reg)
{
    outb(0x70, reg);
    return inb(0x71);
}

static uint32_t bcd(uint8_t v)
{
    return (v & 0x0F) + (v >> 4) * 10;
}

/* Days since 1970-01-01 (proleptic Gregorian). */
static uint32_t days_since_epoch(uint32_t y, uint32_t m, uint32_t d)
{
    if (m <= 2) {                       /* count from March: leap day last */
        y--;
        m += 12;
    }
    return 365 * y + y / 4 - y / 100 + y / 400 + (153 * (m - 3) + 2) / 5 + d - 719469;
}

uint32_t rtc_read_epoch(void)
{
    while (cmos(0x0A) & 0x80)           /* wait out an update in progress */
        ;
    uint32_t sec = cmos(0x00), min = cmos(0x02), hour = cmos(0x04);
    uint32_t day = cmos(0x07), mon = cmos(0x08), year = cmos(0x09);
    uint8_t status = cmos(0x0B);
    int pm = hour & 0x80;
    hour &= 0x7F;
    if (!(status & 0x04)) {             /* BCD (the usual case) */
        sec = bcd(sec); min = bcd(min); hour = bcd(hour);
        day = bcd(day); mon = bcd(mon); year = bcd(year);
    }
    if (!(status & 0x02) && pm)         /* 12-hour clock */
        hour = (hour % 12) + 12;
    year += year < 70 ? 2000 : 1900;
    return days_since_epoch(year, mon, day) * 86400 + hour * 3600 + min * 60 + sec;
}
