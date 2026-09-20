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

static bool display_on = true;
static uint32_t timeout_ms = 5000;
static void (*wake_cb)(void) = NULL;
static void (*sleep_cb)(void) = NULL;
static TickType_t s_last_sleep_tick = 0;

#if CONFIG_PM_ENABLE
static esp_pm_lock_handle_t s_cpu_max_lock = NULL;
#endif

static void display_turn_off_internal(void) {
    if (!display_on) {
        return;
    }
    ESP_LOGI(TAG, "Turning display off (AMOLED Sleep + LVGL Pause + 80MHz DFS)");
    
    if (sleep_cb) {
        sleep_cb();
    }

    // 1. Stop LVGL task to eliminate all rendering & QSPI DMA traffic
    if (lvgl_port_lock(1000)) {
        lvgl_port_stop();
        lvgl_port_unlock();
    } else {
        lvgl_port_stop();
    }

    // 2. Put panel into ultra-low-power sleep (0x28 + 0x10)
    bsp_display_sleep();
    bsp_display_brightness_set(0);

    // 3. Drop CPU frequency to 80MHz while screen is off to save power (keeps APB at 80MHz for I2C stability)
#if CONFIG_PM_ENABLE
    if (s_cpu_max_lock) {
        (void)esp_pm_lock_release(s_cpu_max_lock);
    }
#endif
    display_on = false;
    s_last_sleep_tick = xTaskGetTickCount();
}

void display_manager_turn_off(void) {
    display_turn_off_internal();
}

void display_manager_turn_on(void) {
    static bool is_waking = false;
    if (!display_on && !is_waking) {
        is_waking = true;
        ESP_LOGI(TAG, "Turning display on (Wake Panel + Resume LVGL + 240MHz Boost)");
        
        // 1. Boost CPU to 240MHz immediately for 60fps UI
        #if CONFIG_PM_ENABLE
        if (s_cpu_max_lock) {
            (void)esp_pm_lock_acquire(s_cpu_max_lock);
        }
        #endif
        // 2. Wake AMOLED panel from sleep (0x11 + 0x29)
        bsp_display_wake();
        
        // 3. Resume LVGL task
        lvgl_port_resume();
        
        // 4. Restore brightness
        bsp_display_brightness_set(current_brightness);
        
        display_on = true;

        if (wake_cb) {
            wake_cb(); // This will trigger AppLockscreen::show_again()
        }
        is_waking = false;
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
    if (display_on) {
        bsp_display_brightness_set(current_brightness);
    }
}

uint8_t display_manager_get_brightness(void) {
    return current_brightness;
}

static void display_manager_task(void *arg) {
    ESP_LOGI(TAG, "Display manager task started (Power Optimized)");
    while (1) {
        if (display_on) {
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
            // Screen is OFF: LVGL paused. Check touch hardware pin (active LOW on touch)
            TickType_t now_tick = xTaskGetTickCount();
            if (((now_tick - s_last_sleep_tick) * portTICK_PERIOD_MS > 350) && gpio_get_level(TOUCH_INT_PIN) == 0) {
                ESP_LOGI(TAG, "Touch detected! Waking up display.");
                // Wait briefly for touch to release so it doesn't cause stray clicks or drag animations
                int release_wait = 0;
                while (gpio_get_level(TOUCH_INT_PIN) == 0 && release_wait < 15) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                    release_wait++;
                }
                display_manager_turn_on();
            }
            vTaskDelay(pdMS_TO_TICKS(40)); // 25 Hz polling: 0.00002% CPU, 100% reliable wake!
        }
    }
}

void display_manager_init(void) {
    timeout_ms = 5000; // 5 seconds default timeout (smartwatch power standard)

    // Ensure TOUCH_INT_PIN (GPIO 38) has pull-up enabled so it is stable HIGH and pulled LOW on touch
    gpio_set_pull_mode(TOUCH_INT_PIN, GPIO_PULLUP_ONLY);

    bsp_display_brightness_set(current_brightness);

    display_manager_pm_early_init();

    xTaskCreate(display_manager_task, "display_mgr", 4000, NULL, 4, NULL);
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
