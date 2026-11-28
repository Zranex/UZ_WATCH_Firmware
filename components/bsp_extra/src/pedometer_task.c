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
#include <time.h>

static const char *TAG = "pedometer_task";

#define PEDO_NVS_NS           "pedometer"
#define PEDO_KEY_STEPS        "steps"
#define PEDO_KEY_DAY          "day"
#define PEDO_SAVE_STEP_DELTA  15       // Auto-commit every 15 new steps
#define PEDO_SAVE_IDLE_US     (20LL * 1000000LL) // Or 20s after last unsaved step
#define PEDO_DAY_CHECK_US     (60LL * 1000000LL) // Check midnight rollover every 60s

// Global step counter (persisted in NVS across reboots, power-offs & firmware flashes!)
static volatile uint32_t step_count = 0;
static volatile uint32_t s_last_saved_steps = 0;
static volatile uint32_t s_saved_day = 0;
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

    nvs_handle_t h = 0;
    if (nvs_open(PEDO_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        (void)nvs_get_u32(h, PEDO_KEY_STEPS, &loaded_steps);
        (void)nvs_get_u32(h, PEDO_KEY_DAY, &loaded_day);
        nvs_close(h);
    }

    // Only auto-reset when calendar strictly moves FORWARD (new day / midnight passed).
    // Never wipe steps if RTC temporarily lost power and fell back to an older date before SNTP sync!
    if (today > 0 && loaded_day > 0 && today > loaded_day) {
        ESP_LOGI(TAG, "New calendar day (%lu -> %lu): resetting daily steps (prev=%lu)",
                 (unsigned long)loaded_day, (unsigned long)today, (unsigned long)loaded_steps);
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
        ESP_LOGI(TAG, "Restored persistent steps=%lu (saved_day=%lu, today=%lu)",
                 (unsigned long)step_count, (unsigned long)s_saved_day, (unsigned long)today);
    }
}

void pedometer_save_now(void)
{
    uint32_t today = pedometer_get_today_ymd();
    uint32_t day_to_save = (today > s_saved_day) ? today : s_saved_day;
    pedometer_persist_to_nvs(step_count, day_to_save, false);
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
            // Calculate vector magnitude
            float mag = sqrtf((acc.x * acc.x) + (acc.y * acc.y) + (acc.z * acc.z));

            // Motion-adaptive delay when screen is off (low power mode)
            if (s_is_low_power) {
                float delta_mag = fabsf(mag - prev_mag);
                if (delta_mag < 0.05f) {
                    if (still_ticks < 50) still_ticks++;
                } else {
                    still_ticks = 0;
                }
                prev_mag = mag;

                // If stationary for > 1 sec (~10 ticks at 100ms), relax polling to 400ms
                if (still_ticks >= 10) {
                    s_pedometer_delay_ms = 400; // Power saving when stationary
                } else {
                    s_pedometer_delay_ms = 100;
                }
            } else {
                still_ticks = 0;
                s_pedometer_delay_ms = 100;
            }

            // Low pass filter to smooth out noise
            filtered_mag = (alpha * mag) + ((1.0f - alpha) * filtered_mag);

            // Peak detection
            if (filtered_mag > PEAK_THRESHOLD) {
                TickType_t current_time = xTaskGetTickCount();

                // Check cooldown to prevent double counting
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
                ESP_LOGI(TAG, "Midnight rollover (%lu -> %lu): resetting daily steps (prev=%lu)",
                         (unsigned long)s_saved_day, (unsigned long)today, (unsigned long)step_count);
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
    // Restore today's persistent step count from NVS before starting the sensor task
    pedometer_load_from_nvs();

    BaseType_t ret = xTaskCreatePinnedToCore(
        pedometer_task,
        "pedometer_task",
        4096,
        NULL,
        2, // Priority 2 (strictly below LVGL=4, BLE=4, TARS=4 so IMU never preempts I2C touch/audio!)
        NULL,
        1  // Run on Core 1 (App Core) to not block Wi-Fi/BLE
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
