#include "../../headers/rtc.h"
#include "io.h"

#define CMOS_ADRESS 0x70
#define CMOS_DATA 0X71



uint8_t rtc_read(uint8_t reg)
{
    outb(CMOS_ADRESS, reg);
    return inb(CMOS_DATA);
}

RTC_TIME rtc_get_time(void)
{
    RTC_TIME    time;

    time.second = bcd_to_bin(rtc_read(0x00));
    time.minute = bcd_to_bin(rtc_read(0x02));
    time.hour = bcd_to_bin(rtc_read(0x04));

    time.day = bcd_to_bin(rtc_read(0x07));
    time.month = bcd_to_bin(rtc_read(0x08));
    time.year = bcd_to_bin(rtc_read(0x09));

    return time;
}

static uint8_t bcd_to_bin(uint8_t value )
{
    return (value & 0x0F) + ((value >> 4) * 10);
}
