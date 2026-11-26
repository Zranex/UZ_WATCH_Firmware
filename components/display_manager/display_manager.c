#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "display_manager.h"
#include "bsp/display.h"
#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
// Power management
#include "sdkconfig.h"
#include "esp_sleep.h"
#if CONFIG_PM_ENABLE
#include "esp_pm.h"
#endif

/*
 * Touch interrupt pin — the FT5x06 controller pulls this LOW on touch.
 * DO NOT reconfigure this pin with gpio_config() — the BSP touch driver
 * already sets it up. We only read its level for wake detection.
 */
#define TOUCH_INT_PIN GPIO_NUM_38
#define TOUCH_I2C_ADDR 0x38

static uint8_t current_brightness = 50; // 50% default: saves ~50% AMOLED power while maintaining high contrast

static const char *TAG = "DisplayMgr";

static volatile bool display_on = true;
static volatile bool s_sleep_requested = false;
static uint32_t timeout_ms = 5000;
static void (*wake_cb)(void) = NULL;
static void (*sleep_cb)(void) = NULL;
static TickType_t s_last_sleep_tick = 0;
static SemaphoreHandle_t s_disp_mutex = NULL;

#if CONFIG_PM_ENABLE
static esp_pm_lock_handle_t s_cpu_max_lock = NULL;
#endif

static void display_turn_off_internal(void) {
    if (!display_on) {
        return;
    }
    if (s_disp_mutex && xSemaphoreTake(s_disp_mutex, pdMS_TO_TICKS(1500)) != pdTRUE) {
        ESP_LOGW(TAG, "display_turn_off_internal skipped: mutex busy");
        return;
    }
    if (!display_on) {
        if (s_disp_mutex) xSemaphoreGive(s_disp_mutex);
        return;
    }
    ESP_LOGI(TAG, "Turning display off (AMOLED Sleep + LVGL Pause + 80MHz DFS)");
    
    if (sleep_cb) {
        sleep_cb();
    }

    // 1 & 2. Serialize LVGL stop + QSPI panel sleep commands INSIDE lvgl_port_lock()
    // so an in-flight QSPI DMA flush (trans_queue_depth=1) can NEVER collide with sleep commands!
    if (lvgl_port_lock(1500)) {
        lvgl_port_stop();
        bsp_display_brightness_set(0);
        bsp_display_sleep();
        lvgl_port_unlock();
    } else {
        ESP_LOGW(TAG, "display_turn_off_internal deferred: lvgl_port_lock busy");
        if (s_disp_mutex) xSemaphoreGive(s_disp_mutex);
        return;
    }

    // 3. Drop CPU frequency to 80MHz while screen is off to save power (keeps APB at 80MHz for I2C stability)
#if CONFIG_PM_ENABLE
    if (s_cpu_max_lock) {
        (void)esp_pm_lock_release(s_cpu_max_lock);
    }
#endif
    display_on = false;
    s_sleep_requested = false;
    s_last_sleep_tick = xTaskGetTickCount();

    if (s_disp_mutex) xSemaphoreGive(s_disp_mutex);
}

void display_manager_turn_off(void) {
    // Always defer sleep to display_manager_task to avoid cross-task QSPI/LVGL deadlocks
    s_sleep_requested = true;
}

void display_manager_request_off(void) {
    // Asynchronous sleep request safe to call from inside LVGL click callbacks without deadlocking LVGL mutex!
    s_sleep_requested = true;
}

void display_manager_turn_on(void) {
    s_sleep_requested = false;
    if (display_on) {
        display_manager_reset_timer();
        return;
    }

    bool did_wake = false;
    if (s_disp_mutex && xSemaphoreTake(s_disp_mutex, pdMS_TO_TICKS(1500)) != pdTRUE) {
        ESP_LOGW(TAG, "display_manager_turn_on skipped: mutex busy");
        return;
    }

    if (!display_on) {
        ESP_LOGI(TAG, "Turning display on (Wake Panel + Resume LVGL + 240MHz Boost)");
        
        // 1. Boost CPU to 240MHz immediately for 60fps UI
        #if CONFIG_PM_ENABLE
        if (s_cpu_max_lock) {
            (void)esp_pm_lock_acquire(s_cpu_max_lock);
        }
        #endif
        // 2, 3, 4. Serialize QSPI wake + brightness + LVGL resume INSIDE lvgl_port_lock()
        if (lvgl_port_lock(1500)) {
            bsp_display_wake();
            bsp_display_brightness_set(current_brightness);
            lvgl_port_resume();
            lvgl_port_unlock();
        } else {
            ESP_LOGW(TAG, "display_manager_turn_on: lvgl lock timeout, forcing wake");
            bsp_display_wake();
            bsp_display_brightness_set(current_brightness);
            lvgl_port_resume();
        }
        
        display_on = true;
        did_wake = true;
    }

    if (s_disp_mutex) xSemaphoreGive(s_disp_mutex);

    if (did_wake && wake_cb) {
        wake_cb(); // Triggered outside s_disp_mutex so wake_cb can safely acquire lvgl_port_lock
    }
    display_manager_reset_timer();
}

bool display_manager_is_on(void) {
    return display_on;
}

void display_manager_reset_timer(void) {
    if (display_on && lvgl_port_lock(10)) {
        lv_disp_trig_activity(NULL);
        lvgl_port_unlock();
    }
}

void display_manager_set_timeout(uint32_t t_ms) {
    timeout_ms = t_ms;
    display_manager_reset_timer();
}

uint32_t display_manager_get_timeout(void) {
    return timeout_ms;
}

void display_manager_set_wake_cb(void (*cb)(void)) {
    wake_cb = cb;
}

void display_manager_set_sleep_cb(void (*cb)(void)) {
    sleep_cb = cb;
}

void display_manager_set_brightness(uint8_t brightness) {
    if (brightness > 100) brightness = 100;
    current_brightness = brightness;
    if (display_on && lvgl_port_lock(200)) {
        bsp_display_brightness_set(current_brightness);
        lvgl_port_unlock();
    }
}

uint8_t display_manager_get_brightness(void) {
    return current_brightness;
}

extern void app_tars_on_triple_tap_wake(void) __attribute__((weak));

static void display_manager_task(void *arg) {
    ESP_LOGI(TAG, "Display manager task started (Triple-Tap Wake + Power Optimized)");
    int tap_count = 0;
    TickType_t first_tap_tick = 0;

    while (1) {
        if (display_on) {
            tap_count = 0;
            if (s_sleep_requested) {
                s_sleep_requested = false;
                display_turn_off_internal();
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            uint32_t inactive = 0;
            if (lvgl_port_lock(50)) {
                inactive = lv_disp_get_inactive_time(NULL);
                lvgl_port_unlock();
            }
            if (inactive >= timeout_ms) {
                display_turn_off_internal();
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        } else {
            // Screen is OFF: LVGL paused. Require 3 distinct taps within 1500ms to wake display.
            TickType_t now_tick = xTaskGetTickCount();
            if (tap_count > 0 && ((now_tick - first_tap_tick) * portTICK_PERIOD_MS > 1500)) {
                tap_count = 0;
            }

            if (((now_tick - s_last_sleep_tick) * portTICK_PERIOD_MS > 350) && gpio_get_level(TOUCH_INT_PIN) == 0) {
                int release_wait = 0;
                while (gpio_get_level(TOUCH_INT_PIN) == 0 && release_wait < 45) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                    release_wait++;
                }

                if (release_wait >= 45) {
                    // Long press / sleeve resting on screen -> ignore and wait for release
                    tap_count = 0;
                    while (gpio_get_level(TOUCH_INT_PIN) == 0) {
                        vTaskDelay(pdMS_TO_TICKS(50));
                    }
                } else {
                    TickType_t tap_tick = xTaskGetTickCount();
                    if (tap_count == 0 || ((tap_tick - first_tap_tick) * portTICK_PERIOD_MS > 1500)) {
                        tap_count = 1;
                        first_tap_tick = tap_tick;
                    } else {
                        tap_count++;
                    }
                    ESP_LOGI(TAG, "Sleep tap %d/3", tap_count);

                    if (tap_count >= 3) {
                        tap_count = 0;
                        ESP_LOGI(TAG, "Triple-tap confirmed! Waking display + TARS 4.5s Quick-Listen.");
                        display_manager_turn_on();
                        if (app_tars_on_triple_tap_wake) {
                            app_tars_on_triple_tap_wake();
                        }
                    } else {
                        vTaskDelay(pdMS_TO_TICKS(25)); // Brief debounce before next tap
                    }
                }
            } else {
                vTaskDelay(pdMS_TO_TICKS(tap_count > 0 ? 20 : 70));
            }
        }
    }
}

void display_manager_init(void) {
    timeout_ms = 5000; // 5 seconds default timeout (smartwatch power standard)

    if (!s_disp_mutex) {
        s_disp_mutex = xSemaphoreCreateMutex();
    }

    // Ensure TOUCH_INT_PIN (GPIO 38) has pull-up enabled so it is stable HIGH and pulled LOW on touch
    gpio_set_pull_mode(TOUCH_INT_PIN, GPIO_PULLUP_ONLY);

    bsp_display_brightness_set(current_brightness);

    display_manager_pm_early_init();

    xTaskCreate(display_manager_task, "display_mgr", 4096, NULL, 4, NULL);
}

void display_manager_pm_early_init(void)
{
#if CONFIG_PM_ENABLE
    esp_pm_config_t pm_config = {
        .max_freq_mhz = 240,
        .min_freq_mhz = 80, // 80MHz keeps APB bus clock at 80MHz, guaranteeing 100% I2C/Touch/PSRAM stability
        .light_sleep_enable = false
    };
    esp_err_t err = esp_pm_configure(&pm_config);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "PM DFS active: 240MHz (Active UI) <-> 80MHz (Screen Sleep)");
    } else {
        ESP_LOGW(TAG, "Failed to configure PM: %s", esp_err_to_name(err));
    }

    if (!s_cpu_max_lock) {
        (void)esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "disp_cpu", &s_cpu_max_lock);
    }
    if (s_cpu_max_lock) {
        (void)esp_pm_lock_acquire(s_cpu_max_lock);
    }
#endif
}
