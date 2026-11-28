#include "pedometer_task.h"
#include "qmi8658.h"
#include "rtc_lib.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "pedometer_task";

#define PEDO_NVS_NS           "pedometer"
#define PEDO_KEY_STEPS        "steps"
#define PEDO_KEY_DAY          "day"
#define PEDO_KEY_YSTEPS       "ysteps"
#define PEDO_KEY_YDAY         "yday"
#define PEDO_SD_LOG_PATH      "/sdcard/step_log.txt" // Strict 8.3 FATFS filename!
#define PEDO_SAVE_STEP_DELTA  15       // Auto-commit every 15 new steps
#define PEDO_SAVE_IDLE_US     (20LL * 1000000LL) // Or 20s after last unsaved step
#define PEDO_DAY_CHECK_US     (60LL * 1000000LL) // Check midnight rollover every 60s

// Global step counter (persisted in NVS across reboots, power-offs & firmware flashes!)
static volatile uint32_t step_count = 0;
static volatile uint32_t s_last_saved_steps = 0;
static volatile uint32_t s_saved_day = 0;
static volatile uint32_t s_prev_day_steps = 0;
static volatile uint32_t s_prev_day_ymd = 0;
static volatile int64_t  s_last_save_us = 0;
static volatile int64_t  s_last_step_us = 0;
static qmi8658_acc_t s_latest_acc = {0};
static volatile bool s_acc_valid = false;

// Algorithm parameters
static volatile uint32_t s_pedometer_delay_ms = 100; // 10 Hz active (halves I2C load), 6.6 Hz sleep, 2.5 Hz still
static volatile bool s_is_low_power = false;
static pedometer_acc_cb_t s_acc_cb = NULL;

void pedometer_register_acc_callback(pedometer_acc_cb_t cb)
{
    s_acc_cb = cb;
}

#define PEAK_THRESHOLD 1.2f             // 1.2 G to trigger a step (Earth gravity is 1.0 G)
#define COOLDOWN_MS 300                 // Minimum time between steps

static uint32_t pedometer_get_today_ymd(void)
{
    struct tm ti = {0};
    if (rtc_get_time(&ti) == ESP_OK) {
        int yr = ti.tm_year + 1900;
        int mo = ti.tm_mon + 1;
        int dy = ti.tm_mday;
        if (yr >= 2024 && yr <= 2099 && mo >= 1 && mo <= 12 && dy >= 1 && dy <= 31) {
            return (uint32_t)(yr * 10000 + mo * 100 + dy);
        }
    }
    return 0;
}

static void pedometer_archive_day_to_sd(uint32_t day_ymd, uint32_t steps)
{
    if (day_ymd < 20240101) return;
    // Read existing lines (up to 7 latest days), update or append, and rewrite cleanly
    uint32_t days[8] = {0};
    uint32_t vals[8] = {0};
    int cnt = 0;

    FILE *f = fopen(PEDO_SD_LOG_PATH, "r");
    if (f) {
        char line[48];
        while (fgets(line, sizeof(line), f) && cnt < 7) {
            unsigned long d = 0, s = 0;
            if (sscanf(line, "%lu:%lu", &d, &s) == 2 && d >= 20240101 && d != day_ymd) {
                days[cnt] = (uint32_t)d;
                vals[cnt] = (uint32_t)s;
                cnt++;
            }
        }
        fclose(f);
    }

    if (cnt >= 7) {
        for (int i = 0; i < 6; i++) {
            days[i] = days[i + 1];
            vals[i] = vals[i + 1];
        }
        cnt = 6;
    }
    days[cnt] = day_ymd;
    vals[cnt] = steps;
    cnt++;

    f = fopen(PEDO_SD_LOG_PATH, "w");
    if (f) {
        for (int i = 0; i < cnt; i++) {
            fprintf(f, "%lu:%lu\n", (unsigned long)days[i], (unsigned long)vals[i]);
        }
        fclose(f);
        ESP_LOGI(TAG, "Archived day %lu (%lu steps) to %s", (unsigned long)day_ymd, (unsigned long)steps, PEDO_SD_LOG_PATH);
    }
}

static void pedometer_persist_to_nvs(uint32_t steps, uint32_t day_ymd, bool force)
{
    if (!force && steps == s_last_saved_steps && (day_ymd == 0 || day_ymd == s_saved_day)) {
        return;
    }
    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(PEDO_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed (%s)", esp_err_to_name(err));
        return;
    }
    nvs_set_u32(h, PEDO_KEY_STEPS, steps);
    if (day_ymd > 0) {
        nvs_set_u32(h, PEDO_KEY_DAY, day_ymd);
    }
    if (s_prev_day_ymd > 0) {
        nvs_set_u32(h, PEDO_KEY_YDAY, s_prev_day_ymd);
        nvs_set_u32(h, PEDO_KEY_YSTEPS, s_prev_day_steps);
    }
    err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        s_last_saved_steps = steps;
        if (day_ymd > 0) s_saved_day = day_ymd;
        s_last_save_us = esp_timer_get_time();
        ESP_LOGI(TAG, "Persisted steps=%lu (day=%lu) to NVS", (unsigned long)steps, (unsigned long)s_saved_day);
    }
}

static void pedometer_load_from_nvs(void)
{
    uint32_t today = pedometer_get_today_ymd();
    uint32_t loaded_steps = 0;
    uint32_t loaded_day = 0;
    uint32_t loaded_ysteps = 0;
    uint32_t loaded_yday = 0;

    nvs_handle_t h = 0;
    if (nvs_open(PEDO_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        (void)nvs_get_u32(h, PEDO_KEY_STEPS, &loaded_steps);
        (void)nvs_get_u32(h, PEDO_KEY_DAY, &loaded_day);
        (void)nvs_get_u32(h, PEDO_KEY_YSTEPS, &loaded_ysteps);
        (void)nvs_get_u32(h, PEDO_KEY_YDAY, &loaded_yday);
        nvs_close(h);
    }
    s_prev_day_steps = loaded_ysteps;
    s_prev_day_ymd = loaded_yday;

    // Only auto-reset when calendar strictly moves FORWARD (new day / midnight passed).
    // Never wipe steps if RTC temporarily lost power and fell back to an older date before SNTP sync!
    if (today > 0 && loaded_day > 0 && today > loaded_day) {
        ESP_LOGI(TAG, "New calendar day (%lu -> %lu): archiving prev=%lu and resetting daily steps",
                 (unsigned long)loaded_day, (unsigned long)today, (unsigned long)loaded_steps);
        s_prev_day_ymd = loaded_day;
        s_prev_day_steps = loaded_steps;
        pedometer_archive_day_to_sd(loaded_day, loaded_steps);
        step_count = 0;
        s_last_saved_steps = 0;
        s_saved_day = today;
        pedometer_persist_to_nvs(0, today, true);
    } else {
        step_count = loaded_steps;
        s_last_saved_steps = loaded_steps;
        s_saved_day = (loaded_day > 0) ? loaded_day : today;
        s_last_save_us = esp_timer_get_time();
        if (today > 0 && loaded_day == 0) {
            pedometer_persist_to_nvs(step_count, today, true);
        }
        ESP_LOGI(TAG, "Restored persistent steps=%lu (saved_day=%lu, today=%lu, prev=%lu@%lu)",
                 (unsigned long)step_count, (unsigned long)s_saved_day, (unsigned long)today,
                 (unsigned long)s_prev_day_steps, (unsigned long)s_prev_day_ymd);
    }
}

void pedometer_save_now(void)
{
    uint32_t today = pedometer_get_today_ymd();
    uint32_t day_to_save = (today > s_saved_day) ? today : s_saved_day;
    pedometer_persist_to_nvs(step_count, day_to_save, false);
}

void pedometer_format_recent_history(char* out, size_t max_len)
{
    if (!out || max_len == 0) return;
    out[0] = '\0';

    // Try SD 7-day log first; fall back to NVS yesterday record if SD unavailable/empty
    FILE *f = fopen(PEDO_SD_LOG_PATH, "r");
    if (f) {
        char line[48];
        size_t pos = 0;
        int n = 0;
        while (fgets(line, sizeof(line), f)) {
            unsigned long d = 0, s = 0;
            if (sscanf(line, "%lu:%lu", &d, &s) == 2 && d >= 20240101) {
                int mo = (int)((d / 100) % 100);
                int dy = (int)(d % 100);
                int w = snprintf(out + pos, max_len - pos, "%s%02d.%02d=%lu",
                                 (n == 0) ? ", GecmisAdim(" : " ", dy, mo, s);
                if (w > 0 && pos + (size_t)w + 2 < max_len) {
                    pos += (size_t)w;
                    n++;
                }
            }
        }
        fclose(f);
        if (n > 0 && pos + 2 < max_len) {
            out[pos++] = ')';
            out[pos] = '\0';
            return;
        }
    }

    if (s_prev_day_ymd >= 20240101) {
        int mo = (int)((s_prev_day_ymd / 100) % 100);
        int dy = (int)(s_prev_day_ymd % 100);
        snprintf(out, max_len, ", OncekiGun(%02d.%02d=%lu)", dy, mo, (unsigned long)s_prev_day_steps);
    }
}

void pedometer_set_low_power(bool enable)
{
    s_is_low_power = enable;
    s_pedometer_delay_ms = enable ? 150 : 100;
    if (enable) {
        pedometer_save_now();
    }
}

static void pedometer_task(void *pvParameter)
{
    qmi8658_acc_t acc;
    float filtered_mag = 1.0f;          // Initialize with 1G
    float alpha = 0.2f;                 // Low pass filter coefficient
    float prev_mag = 1.0f;
    int still_ticks = 0;

    TickType_t last_step_time = 0;
    int64_t last_day_check_us = esp_timer_get_time();

    ESP_LOGI(TAG, "Pedometer task started (Motion-Adaptive + NVS Persistent, steps=%lu)", (unsigned long)step_count);

    while (1) {
        if (qmi8658_read_acc(&acc) == ESP_OK) {
            s_latest_acc = acc;
            s_acc_valid = true;
            if (s_acc_cb) {
                s_acc_cb(&acc);
            }
            float mag = sqrtf((acc.x * acc.x) + (acc.y * acc.y) + (acc.z * acc.z));

            if (s_is_low_power) {
                float delta_mag = fabsf(mag - prev_mag);
                if (delta_mag < 0.05f) {
                    if (still_ticks < 50) still_ticks++;
                } else {
                    still_ticks = 0;
                }
                prev_mag = mag;

                if (still_ticks >= 10) {
                    s_pedometer_delay_ms = 400;
                } else {
                    s_pedometer_delay_ms = 100;
                }
            } else {
                still_ticks = 0;
                s_pedometer_delay_ms = 100;
            }

            filtered_mag = (alpha * mag) + ((1.0f - alpha) * filtered_mag);

            if (filtered_mag > PEAK_THRESHOLD) {
                TickType_t current_time = xTaskGetTickCount();
                if ((current_time - last_step_time) * portTICK_PERIOD_MS > COOLDOWN_MS) {
                    step_count++;
                    last_step_time = current_time;
                    s_last_step_us = esp_timer_get_time();
                    ESP_LOGD(TAG, "Step detected! Total: %lu", (unsigned long)step_count);
                }
            }
        }

        // Periodic NVS auto-commit & midnight rollover check (wear-safe)
        int64_t now_us = esp_timer_get_time();
        uint32_t cur_steps = step_count;
        uint32_t saved_steps = s_last_saved_steps;
        if (cur_steps != saved_steps) {
            uint32_t delta = (cur_steps >= saved_steps) ? (cur_steps - saved_steps) : PEDO_SAVE_STEP_DELTA;
            if (delta >= PEDO_SAVE_STEP_DELTA ||
                (now_us - s_last_step_us >= PEDO_SAVE_IDLE_US && now_us - s_last_save_us >= PEDO_SAVE_IDLE_US)) {
                pedometer_save_now();
            }
        }

        if (now_us - last_day_check_us >= PEDO_DAY_CHECK_US) {
            last_day_check_us = now_us;
            uint32_t today = pedometer_get_today_ymd();
            if (today > 0 && s_saved_day > 0 && today > s_saved_day) {
                ESP_LOGI(TAG, "Midnight rollover (%lu -> %lu): archiving prev=%lu and resetting daily steps",
                         (unsigned long)s_saved_day, (unsigned long)today, (unsigned long)step_count);
                s_prev_day_ymd = s_saved_day;
                s_prev_day_steps = step_count;
                pedometer_archive_day_to_sd(s_saved_day, step_count);
                step_count = 0;
                s_saved_day = today;
                pedometer_persist_to_nvs(0, today, true);
            } else if (today > 0 && s_saved_day == 0) {
                pedometer_persist_to_nvs(step_count, today, true);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(s_pedometer_delay_ms));
    }
}

esp_err_t pedometer_task_init(void)
{
    pedometer_load_from_nvs();

    BaseType_t ret = xTaskCreatePinnedToCore(
        pedometer_task,
        "pedometer_task",
        4096,
        NULL,
        2,
        NULL,
        1
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create pedometer task");
        return ESP_FAIL;
    }

    return ESP_OK;
}

uint32_t pedometer_get_steps(void)
{
    return step_count;
}

void pedometer_reset_steps(void)
{
    step_count = 0;
    uint32_t today = pedometer_get_today_ymd();
    pedometer_persist_to_nvs(0, (today > 0) ? today : s_saved_day, true);
}

esp_err_t pedometer_get_latest_acc(qmi8658_acc_t *acc)
{
    if (!acc) return ESP_ERR_INVALID_ARG;
    if (!s_acc_valid) {
        return qmi8658_read_acc(acc);
    }
    *acc = s_latest_acc;
    return ESP_OK;
}
