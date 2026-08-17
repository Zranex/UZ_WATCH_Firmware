/*
 * UZ WATCH v3 — Temel Firmware + Calculator + Calibration + IMU Wake
 * Touch wake aktif, uygulama manuel installApp ile yükleniyor
 */

#include "bsp/esp-bsp.h"
#include "esp_brookesia.hpp"
#ifdef ESP_UTILS_LOG_TAG
#   undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "UZ-WATCH"
#include "esp_lib_utils.h"
#include "dark/stylesheet.h"

#include "display_manager.h"
#include "bsp_board_extra.h"
extern "C" {
#include "rtc_lib.h"
#include "qmi8658.h"
}
#include "esp_brookesia_app_calculator.hpp"
#include "esp_brookesia_app_game_2048.hpp"
#include "esp_brookesia_app_settings.hpp"
#include "app_watchface.hpp"
#include "app_notifications.hpp"
#include "app_lockscreen.hpp"
#include "app_calibration.hpp"
#include "app_media_player.hpp"

extern "C" {
#include "ble_manager.h"
#include "wifi_manager.h"
}

using namespace esp_brookesia;
using namespace esp_brookesia::gui;
using namespace esp_brookesia::systems::phone;

#define LVGL_PORT_INIT_CONFIG() \
    {                               \
        .task_priority = 4,       \
        .task_stack = 20 * 1024,       \
        .task_affinity = 1,      \
        .task_max_sleep_ms = 500, \
        .timer_period_ms = 5,     \
    }



static void imu_task(void *pvParameter) {
    while(1) {
        // Screen timeout is now fully handled by display_manager.c
        // Wrist wake logic
        qmi8658_acc_t acc;
        if (qmi8658_read_acc(&acc) == ESP_OK) {
            float bx, by, bz;
            AppCalibration::get_calibrated_baseline(&bx, &by, &bz);
            
            float dx = acc.x - bx;
            float dy = acc.y - by;
            float dz = acc.z - bz;
            float dist_sq = dx*dx + dy*dy + dz*dz;
            
            // If the watch is held in a position close to the calibrated baseline (within ~0.3g threshold)
            if (dist_sq < 0.3f && !display_manager_is_on()) {
                display_manager_turn_on();
                ESP_UTILS_LOGI("Wrist tilt detected! Display ON.");
                
                bsp_display_lock(0);
                lv_disp_trig_activity(NULL); // Reset LVGL inactivity timer
                bsp_display_unlock();
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(100)); // Run every 100ms
    }
}

extern "C" void app_main(void)
{
    ESP_UTILS_LOGI("UZ WATCH v3 — Baslatiliyor");

    /* 1. Ekrani baslat */
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = LVGL_PORT_INIT_CONFIG()
    };
    ESP_UTILS_CHECK_NULL_EXIT(bsp_display_start_with_config(&cfg), "Start display failed");

    ESP_LOGW(ESP_UTILS_LOG_TAG, "--- MEMORY AFTER DISPLAY INIT ---");
    ESP_LOGW(ESP_UTILS_LOG_TAG, "INTERNAL Free: %u, Largest: %u", heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    ESP_LOGW(ESP_UTILS_LOG_TAG, "DMA Free: %u", heap_caps_get_free_size(MALLOC_CAP_DMA));
    ESP_LOGW(ESP_UTILS_LOG_TAG, "PSRAM Free: %u, Largest: %u", heap_caps_get_free_size(MALLOC_CAP_SPIRAM), heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    ESP_LOGW(ESP_UTILS_LOG_TAG, "---------------------------------");

    /* 2. Parlaklik */
    bsp_display_brightness_set(100);

    /* 3. Ekstra donanim (RTC vb.) — ekrandan SONRA */
    bsp_extra_init();
    rtc_start();
    
    struct tm timeinfo;
    rtc_get_time(&timeinfo);
    if (timeinfo.tm_year < 120) {
        timeinfo.tm_year = 127;
        timeinfo.tm_mon = 4;
        timeinfo.tm_mday = 24;
        timeinfo.tm_hour = 20;
        timeinfo.tm_min = 14;
        timeinfo.tm_sec = 34;
        rtc_set_time(&timeinfo);
    }

    /* 4. LVGL kilit */
    LvLock::registerCallbacks([](int timeout_ms) {
        if (timeout_ms < 0) timeout_ms = 0;
        else if (timeout_ms == 0) timeout_ms = 1;
        ESP_UTILS_CHECK_FALSE_RETURN(bsp_display_lock(timeout_ms), false, "Lock failed");
        return true;
    }, []() {
        bsp_display_unlock();
        return true;
    });

    display_manager_init();
    
    display_manager_set_wake_cb([]() {
        // display_manager.c already sets bsp_display_brightness_set(100) before calling this
        bsp_display_lock(0);
        lv_disp_trig_activity(NULL);
        AppLockscreen::show_again();
        bsp_display_unlock();
    });

    display_manager_set_sleep_cb([]() {
        bsp_display_lock(0);
        // Force close AppMediaPlayer if it is running when screen turns off.
        // This ensures the user returns to the main launcher screen after unlocking.
        AppMediaPlayer::force_close();
        bsp_display_unlock();
    });

    /* 5. Phone nesnesi */
    Phone* phone = new (std::nothrow) Phone();
    ESP_UTILS_CHECK_NULL_EXIT(phone, "Create phone failed");

    /* 6. Stylesheet */
    Stylesheet* stylesheet = new (std::nothrow) Stylesheet(STYLESHEET_410_502_DARK);
    if (stylesheet != nullptr) {
        ESP_UTILS_LOGI("Using stylesheet (%s)", stylesheet->core.name);
        ESP_UTILS_CHECK_FALSE_EXIT(phone->addStylesheet(stylesheet), "Add stylesheet failed");
        ESP_UTILS_CHECK_FALSE_EXIT(phone->activateStylesheet(stylesheet), "Activate stylesheet failed");
        delete stylesheet;
    }

    {
        LvLockGuard gui_guard;

        ESP_UTILS_CHECK_FALSE_EXIT(phone->begin(), "Begin failed");
        
        /* Kilit Ekrani */
        AppLockscreen::show(phone);

        /* 7. Uygulamalari yukle */
        esp_brookesia::apps::Watchface *watchface = new (std::nothrow) esp_brookesia::apps::Watchface();
        ESP_UTILS_CHECK_NULL_EXIT(watchface, "Create Watchface failed");
        if (phone->installApp(watchface) >= 0) {
            ESP_UTILS_LOGI("Watchface yuklendi");
        }
        
        AppCalibration *calibApp = new (std::nothrow) AppCalibration();
        ESP_UTILS_CHECK_NULL_EXIT(calibApp, "Create CalibApp failed");
        if (phone->installApp(calibApp) >= 0) {
            ESP_UTILS_LOGI("Calibration App yuklendi");
        }

        AppMediaPlayer *mediaApp = new (std::nothrow) AppMediaPlayer();
        ESP_UTILS_CHECK_NULL_EXIT(mediaApp, "Create MediaApp failed");
        if (phone->installApp(mediaApp) >= 0) {
            ESP_UTILS_LOGI("Media Player App yuklendi");
        }

        /* 8. Saat guncelleme zamanlayicisi */
        lv_timer_create([](lv_timer_t* t) {
            time_t now;
            struct tm timeinfo;
            Phone* phone = (Phone*)t->user_data;
            ESP_UTILS_CHECK_NULL_EXIT(phone, "Invalid phone");
            time(&now);
            localtime_r(&now, &timeinfo);
            ESP_UTILS_CHECK_FALSE_EXIT(
                phone->getDisplay().getStatusBar()->setClock(timeinfo.tm_hour, timeinfo.tm_min),
                "Refresh status bar failed"
            );
        }, 1000, phone);
    }

    // Start IMU Background Task
    xTaskCreate(imu_task, "imu_task", 4096, NULL, 5, NULL);

    ESP_UTILS_LOGI("UZ WATCH v3 - Sistem Hazir!");
    ble_manager_init();
}
