#include <devices.h>
#include <console.h>
#include <string.h>

/* MC146818 CMOS real-time clock */
#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

static u8 cmos_read(u8 reg)
{
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}

static int update_in_progress(void)
{
    return cmos_read(0x0A) & 0x80;
}

static u8 bcd_to_bin(u8 v)
{
    return (u8)((v & 0x0F) + ((v >> 4) * 10));
}

static int is_bcd(void)
{
    return !(cmos_read(0x0B) & 0x04);
}

void rtc_read(struct hw_profile *p)
{
    /* Wait for the RTC to finish an in-progress update before reading. */
    u32 guard = 0;
    while (update_in_progress() && ++guard < 1000000) cpu_relax();
    if (guard >= 1000000) return;

    u8 sec = cmos_read(0x00), min = cmos_read(0x02), hour = cmos_read(0x04);
    u8 day = cmos_read(0x07), mon = cmos_read(0x08), year = cmos_read(0x09);

    if (is_bcd()) {
        sec = bcd_to_bin(sec);
        min = bcd_to_bin(min);
        hour = (u8)((hour & 0x0F) + (((hour & 0x70) >> 4) * 10));
        day = bcd_to_bin(day);
        mon = bcd_to_bin(mon);
        year = bcd_to_bin(year);
    }
    if (!(cmos_read(0x0B) & 0x02)) hour = (u8)(((hour & 0x7F) + 12) % 24);

    int full_year = 2000 + year;
    if (full_year < 1990) full_year += 100;

    /* Sanity-check so a bogus/unset CMOS clock is not reported as valid. */
    if (mon < 1 || mon > 12 || day < 1 || day > 31 || hour > 23 ||
        min > 59 || sec > 59) {
        return;
    }

    p->has_rtc = 1;
    p->rtc_year = full_year;
    p->rtc_month = mon;
    p->rtc_day = day;
    p->rtc_hour = hour;
    p->rtc_minute = min;
    p->rtc_second = sec;
}

/* Re-exported so the shell can print the wall clock without a full discovery. */
void rtc_now_string(char *buf, usize n)
{
    struct hw_profile tmp;
    tmp.has_rtc = 0;
    rtc_read(&tmp);
    if (tmp.has_rtc)
        snprintf(buf, n, "%04d-%02d-%02d %02d:%02d:%02d",
                 tmp.rtc_year, tmp.rtc_month, tmp.rtc_day,
                 tmp.rtc_hour, tmp.rtc_minute, tmp.rtc_second);
    else
        snprintf(buf, n, "(RTC unavailable)");
}
