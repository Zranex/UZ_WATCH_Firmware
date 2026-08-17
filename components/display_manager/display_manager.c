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

static uint8_t current_brightness = 100;

static const char *TAG = "DisplayMgr";

static bool display_on = true;
static uint32_t timeout_ms;
static void (*wake_cb)(void) = NULL;
static void (*sleep_cb)(void) = NULL;
#if CONFIG_PM_ENABLE
static esp_pm_lock_handle_t s_no_ls_lock = NULL;
#endif

static void display_turn_off_internal(void) {
    if (!display_on) {
        return;
    }
    ESP_LOGI(TAG, "Turning display off");
    
    // We do NOT stop LVGL or disable indev.
    // This allows LVGL to natively detect the next touch to wake up!
    
    // Turn off brightness (AMOLED — no backlight, brightness controls panel)
    bsp_display_brightness_set(0);
    // Allow automatic light sleep while the screen is off
#if CONFIG_PM_ENABLE
    if (s_no_ls_lock) {
        (void)esp_pm_lock_release(s_no_ls_lock);
    }
#endif
    display_on = false;
    
    if (sleep_cb) {
        sleep_cb();
    }
}

void display_manager_turn_off(void) {
    display_turn_off_internal();
}

void display_manager_turn_on(void) {
    if (!display_on) {
        ESP_LOGI(TAG, "Turning display on");
        // Restore brightness
        bsp_display_brightness_set(current_brightness);
        
        if (wake_cb) {
            wake_cb(); // This will trigger AppLockscreen::show_again()
        }
        
        display_on = true;
    }
    // Prevent light sleep while actively displaying UI
#if CONFIG_PM_ENABLE
    if (s_no_ls_lock) {
        (void)esp_pm_lock_acquire(s_no_ls_lock);
    }
#endif
    display_manager_reset_timer();
}

bool display_manager_is_on(void) {
    return display_on;
}

void display_manager_reset_timer(void) {
    lv_disp_trig_activity(NULL);
}

void display_manager_set_timeout(uint32_t t_ms) {
    timeout_ms = t_ms;
    display_manager_reset_timer(); // reset timer when setting changes
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
    ESP_LOGI(TAG, "Display manager task started (native LVGL wake)");
    while (1) {
        uint32_t inactive = 0;
        if (lvgl_port_lock(0)) {
            inactive = lv_disp_get_inactive_time(NULL);
            lvgl_port_unlock();
        }
        
        if (display_on) {
            if (inactive >= timeout_ms) {
                display_turn_off_internal();
            }
        } else {
            // Screen is off — LVGL is still running and polling touch
            if (inactive < timeout_ms) {
                display_manager_turn_on();
            } else if (gpio_get_level(TOUCH_INT_PIN) == 0) {
                // Hardware interrupt from touch controller!
                ESP_LOGI(TAG, "Touch INT detected! Waking up display.");
                display_manager_turn_on();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void display_manager_init(void) {
    timeout_ms = 30000;

    /*
     * NOTE: Do NOT call gpio_config() on TOUCH_INT_PIN here!
     * The BSP touch driver (FT5x06) already configures GPIO_38.
     * Reconfiguring it would break touch input for LVGL.
     */

    bsp_display_brightness_set(current_brightness);

    xTaskCreate(display_manager_task, "display_mgr", 4000, NULL, 4, NULL);
}

void display_manager_pm_early_init(void)
{
#if CONFIG_PM_ENABLE
    if (!s_no_ls_lock) {
        (void)esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "display", &s_no_ls_lock);
    }
    if (s_no_ls_lock) {
        (void)esp_pm_lock_acquire(s_no_ls_lock);
    }
#else
    (void)0;
#endif
}
