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
#include "pedometer_task.h"
}
#include "esp_brookesia_app_calculator.hpp"
#include "esp_brookesia_app_game_2048.hpp"
#include "esp_brookesia_app_settings.hpp"
#include "app_watchface.hpp"
#include "ui_notification_banner.hpp"
#include "app_lockscreen.hpp"
#include "app_calibration.hpp"
#include "app_media_player.hpp"
#include "app_weather.hpp"
#include "app_notifications_custom.hpp"
#include "app_activity.hpp"
#include "app_smart_home.hpp"
#include "app_local_music.hpp"
#include "app_voice_recorder.hpp"
#include "app_find_phone.hpp"
#include "app_flashlight.hpp"
#include "app_agenda.hpp"
#include "app_transit.hpp"
#include "app_airdrop.hpp"

extern "C" {
#include "ble_manager.h"
#include "wifi_manager.h"
#include "esp_wifi.h"
#include "bsp/esp32_s3_touch_amoled_2_06.h"
}

using namespace esp_brookesia;
using namespace esp_brookesia::gui;
using namespace esp_brookesia::systems::phone;

// Global audio codec handle
esp_codec_dev_handle_t spk_codec_dev = NULL;

#define LVGL_PORT_INIT_CONFIG() \
    {                               \
        .task_priority = 4,       \
        .task_stack = 20 * 1024,       \
        .task_affinity = 1,      \
        .task_max_sleep_ms = 500, \
        .timer_period_ms = 5,     \
    }

static void imu_task(void *pvParameter) {
    static bool was_tilted = false;
    static int stable_tilt_count = 0;
    while(1) {
        bool is_display_on = display_manager_is_on();
        
        qmi8658_acc_t acc;
        if (pedometer_get_latest_acc(&acc) == ESP_OK) {
            float bx, by, bz;
            AppCalibration::get_calibrated_baseline(&bx, &by, &bz);
            
            float dx = acc.x - bx;
            float dy = acc.y - by;
            float dz = acc.z - bz;
            float dist_sq = dx*dx + dy*dy + dz*dz;
            
            // Tightened threshold: radius <= 0.40G (0.16f dist_sq) instead of 0.50G (0.25f)
            // Stillness verification: requires wrist to stay in viewing angle for >= 2 consecutive samples (~500ms)
            // Eliminates false triggers while walking, writing with pen, or carrying a backpack at school
            if (dist_sq < 0.16f) {
                if (!was_tilted) {
                    stable_tilt_count++;
                    if (stable_tilt_count >= 2) {
                        was_tilted = true;
                        stable_tilt_count = 0;
                        if (!is_display_on) {
                            display_manager_turn_on();
                            ESP_UTILS_LOGI("Wrist tilt stable viewing confirmed! Display ON.");
                        }
                    }
                }
            } else if (dist_sq > 0.32f) {
                // Hysteresis: arm has moved away from viewing position
                was_tilted = false;
                stable_tilt_count = 0;
            } else {
                stable_tilt_count = 0;
            }
        }
        
        // Low power delay: 250ms when screen off, 200ms when screen on
        vTaskDelay(pdMS_TO_TICKS(is_display_on ? 200 : 250));
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

    // Power Saving: Baslangic parlakligi %50
    bsp_display_brightness_set(50);

    /* 3. Ekstra donanim (RTC vb.) — ekrandan SONRA */
    bsp_extra_init();
    pedometer_task_init();
    rtc_start();
    
    /* Initialize SD Card */
    esp_err_t sd_err = bsp_sdcard_mount();
    if(sd_err == ESP_OK) {
        ESP_UTILS_LOGI("SD Card mounted successfully!");
    } else {
        ESP_UTILS_LOGE("Failed to mount SD Card: %d", sd_err);
    }

    /* Initialize Audio Speaker */
    esp_err_t audio_err = bsp_audio_init(NULL);
    if(audio_err == ESP_OK) {
        spk_codec_dev = bsp_audio_codec_speaker_init();
        if(spk_codec_dev) {
            ESP_UTILS_LOGI("Speaker initialized successfully!");
            esp_codec_dev_set_out_vol(spk_codec_dev, 70); // Set default volume to 70%
            bsp_audio_power_amp_enable(false); // Shut off PA when silent to save 20mA
        } else {
            ESP_UTILS_LOGE("Failed to initialize speaker codec!");
        }
    } else {
        ESP_UTILS_LOGE("Failed to initialize audio I2S: %d", audio_err);
    }

    struct tm timeinfo;
    rtc_get_time(&timeinfo);
    if (timeinfo.tm_year < 120) {
        timeinfo.tm_year = 126; // 2026
        timeinfo.tm_mon = 8;    // September
        timeinfo.tm_mday = 13;
        timeinfo.tm_hour = 18;
        timeinfo.tm_min = 35;
        timeinfo.tm_sec = 0;
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
        if (wifi_manager_is_active()) {
            esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        }
        if (bsp_display_lock(1000)) {
            lv_disp_trig_activity(NULL);
            AppLockscreen::show_again();
            bsp_display_unlock();
        }
    });

    display_manager_set_sleep_cb([]() {
        if (wifi_manager_is_active()) {
            esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
        }
        if (bsp_display_lock(1000)) {
            AppMediaPlayer::force_close();
            bsp_display_unlock();
        }
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
        phone->installApp(watchface);
        
        AppCalibration *calibApp = new (std::nothrow) AppCalibration();
        phone->installApp(calibApp);

        AppMediaPlayer *mediaApp = new (std::nothrow) AppMediaPlayer();
        phone->installApp(mediaApp);
        AppWeather *weatherApp = new (std::nothrow) AppWeather();
        phone->installApp(weatherApp);
        
        AppLocalMusic *localMusicApp = new (std::nothrow) AppLocalMusic();
        phone->installApp(localMusicApp);

        AppNotificationsCustom *notifApp = new (std::nothrow) AppNotificationsCustom();
        phone->installApp(notifApp);

        AppActivity *activityApp = new (std::nothrow) AppActivity();
        phone->installApp(activityApp);
        
        AppSmartHome *smartHomeApp = new (std::nothrow) AppSmartHome();
        phone->installApp(smartHomeApp);

        AppVoiceRecorder *voiceRecApp = new (std::nothrow) AppVoiceRecorder();
        phone->installApp(voiceRecApp);
        
        AppFindPhone *findPhoneApp = new (std::nothrow) AppFindPhone();
        phone->installApp(findPhoneApp);
        
                AppFlashlight *flashlightApp = new (std::nothrow) AppFlashlight();
        phone->installApp(flashlightApp);

        AppAgenda *agendaApp = new (std::nothrow) AppAgenda();
        phone->installApp(agendaApp);

        AppTransit *transitApp = new (std::nothrow) AppTransit();
        phone->installApp(transitApp);

        AppAirDrop *airdropApp = new (std::nothrow) AppAirDrop();
        if(airdropApp) phone->installApp(airdropApp);

        /* Notifications UI Init */
        AppNotifications::init();

        /* 8. Durum Cubugu Guncelleme (Saat, Wi-Fi, Pil) */
        // Acilista aninda ilk guncelleme (0. saniye)
        {
            time_t now;
            struct tm timeinfo;
            time(&now);
            localtime_r(&now, &timeinfo);
            phone->getDisplay().getStatusBar()->setClock(timeinfo.tm_hour, timeinfo.tm_min);
            StatusBar::WifiState ws = StatusBar::WifiState::DISCONNECTED;
            if (wifi_manager_is_connected()) ws = StatusBar::WifiState::SIGNAL_3;
            else if (wifi_manager_is_active()) ws = StatusBar::WifiState::SIGNAL_1;
            phone->getDisplay().getStatusBar()->setWifiIconState(ws);
            int percent = bsp_battery_get_percent();
            bool charging = bsp_battery_is_charging();
            if (percent >= 0 && percent <= 100) {
                phone->getDisplay().getStatusBar()->setBatteryPercent(charging, percent);
            }
        }

        // Periyodik guncelleme (Saat her saniye, Wi-Fi 2s, Pil 5s)
        lv_timer_create([](lv_timer_t* t) {
            Phone* phone = (Phone*)t->user_data;
            time_t now;
            struct tm timeinfo;
            time(&now);
            localtime_r(&now, &timeinfo);
            phone->getDisplay().getStatusBar()->setClock(timeinfo.tm_hour, timeinfo.tm_min);
            
            // Wi-Fi durumu (her saniye kontrol edilir - aninda tepki)
            StatusBar::WifiState ws = StatusBar::WifiState::DISCONNECTED;
            if (wifi_manager_is_connected()) ws = StatusBar::WifiState::SIGNAL_3;
            else if (wifi_manager_is_active() && wifi_manager_get_state() != WIFI_STATE_FAILED) ws = StatusBar::WifiState::SIGNAL_1;
            phone->getDisplay().getStatusBar()->setWifiIconState(ws);

            // Pil durumu (her 5 saniyede bir)
            static int bat_tick = 0;
            if (++bat_tick >= 5) {
                bat_tick = 0;
                int percent = bsp_battery_get_percent();
                bool charging = bsp_battery_is_charging();
                if (percent >= 0 && percent <= 100) {
                    phone->getDisplay().getStatusBar()->setBatteryPercent(charging, percent);
                }
            }
        }, 1000, phone);
    }

    // Start IMU Background Task
    xTaskCreate(imu_task, "imu_task", 4096, NULL, 5, NULL);

    ESP_UTILS_LOGI("UZ WATCH v3 - Sistem Hazir!");
    ble_manager_init();
    // Wi-Fi is strictly on-demand (Settings/AirDrop) to achieve 10-12+ hour battery life
}


