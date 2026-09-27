#ifndef RTC_H
#define RTC_H

#include <stdint.h>

typedef struct {

    int second;
    int minute;
    int hour;

    int day;
    int month;
    int year;

}   RTC_TIME;

uint8_t rtc_read(uint8_t reg);

static uint8_t bcd_to_bin(uint8_t value );

static uint8_t bcd_to_bin(uint8_t value );

RTC_TIME rtc_get_time(void);

#endif
