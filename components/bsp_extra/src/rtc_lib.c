#include "rtc_lib.h"
#include "pcf85063a.h"
#include <time.h>
#include <sys/time.h>
#include "esp_timer.h"

static struct tm current_time;
static esp_timer_handle_t rtc_timer = NULL;

static const char *weekdays[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char *weekdaysshort[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
static const char *months[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};

static void rtc_sync_from_hw_cb(void *arg)
{
    struct tm hw_tm = {0};
    if (pcf85063a_get_time(&hw_tm) == ESP_OK) {
        hw_tm.tm_isdst = 0;
        current_time = hw_tm;
        time_t t = mktime(&hw_tm);
        if (t > 0) {
            struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
            settimeofday(&tv, NULL);
        }
    }
}

static void refresh_cached_from_systime(void)
{
    time_t now = 0;
    time(&now);
    localtime_r(&now, &current_time);
}

esp_err_t rtc_start(void)
{
    esp_err_t ret = pcf85063a_init();
    if (ret != ESP_OK) {
        return ret;
    }

    // Immediately sync hardware RTC time to ESP32 system clock at boot
    rtc_sync_from_hw_cb(NULL);

    if (rtc_timer == NULL) {
        const esp_timer_create_args_t rtc_timer_args = {
            .callback = &rtc_sync_from_hw_cb,
            .name = "rtc_hw_sync",
            .skip_unhandled_events = true
        };

        ret = esp_timer_create(&rtc_timer_args, &rtc_timer);
        if (ret != ESP_OK) {
            return ret;
        }

        // Re-sync against hardware PCF85063A every 5 minutes instead of every 1 second!
        // Saves 3,588 background I2C wakeups per hour while keeping sub-second precision via ESP32 XTAL.
        return esp_timer_start_periodic(rtc_timer, 300ULL * 1000000ULL);
    }
    return ESP_OK;
}

esp_err_t rtc_get_time(struct tm *out_time)
{
    if (!out_time) return ESP_ERR_INVALID_ARG;
    refresh_cached_from_systime();
    *out_time = current_time;
    return ESP_OK;
}

esp_err_t rtc_set_time(const struct tm *new_time)
{
    if (!new_time) return ESP_ERR_INVALID_ARG;
    esp_err_t err = pcf85063a_set_time(new_time);
    struct tm tmp = *new_time;
    tmp.tm_isdst = 0;
    time_t t = mktime(&tmp);
    if (t > 0) {
        struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        localtime_r(&t, &current_time);
    } else {
        current_time = *new_time;
    }
    return err;
}

int rtc_get_hour(void)
{
    refresh_cached_from_systime();
    return current_time.tm_hour;
}

int rtc_get_minute(void)
{
    refresh_cached_from_systime();
    return current_time.tm_min;
}

int rtc_get_second(void)
{
    refresh_cached_from_systime();
    return current_time.tm_sec;
}

int rtc_get_day(void)
{
    refresh_cached_from_systime();
    return current_time.tm_mday;
}

int rtc_get_month(void)
{
    refresh_cached_from_systime();
    return current_time.tm_mon + 1;
}

int rtc_get_year(void)
{
    refresh_cached_from_systime();
    return current_time.tm_year + 1900;
}

const char *rtc_get_weekday_string(void)
{
    refresh_cached_from_systime();
    int w = current_time.tm_wday;
    return (w >= 0 && w < 7) ? weekdays[w] : weekdays[0];
}

const char *rtc_get_weekday_short_string(void)
{
    refresh_cached_from_systime();
    int w = current_time.tm_wday;
    return (w >= 0 && w < 7) ? weekdaysshort[w] : weekdaysshort[0];
}

const char *rtc_get_month_string(void)
{
    refresh_cached_from_systime();
    int m = current_time.tm_mon;
    return (m >= 0 && m < 12) ? months[m] : months[0];
}
