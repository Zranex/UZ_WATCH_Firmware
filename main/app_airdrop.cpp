#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
extern "C" const lv_image_dsc_t icon_airdrop;

#include "app_airdrop.hpp"
#include "airdrop_manager.hpp"
#include "app_tars.hpp"
#include "esp_log.h"
#include "wifi_manager.h"
#include "ble_manager.h"
#include "display_manager.h"
#include <string.h>

static const char *TAG = "AppAirDrop";

enum AirDropCmd : uint8_t {
    AIRDROP_CMD_START_AP = 1,
    AIRDROP_CMD_START_STA = 2,
    AIRDROP_CMD_STOP_ALL = 3,
};

enum AirDropWorkerStatus : int {
    AD_STATUS_IDLE = 0,
    AD_STATUS_STARTING_AP = 1,
    AD_STATUS_STARTING_STA = 2,
    AD_STATUS_READY_AP = 3,
    AD_STATUS_READY_STA = 4,
    AD_STATUS_STOPPING = 5,
    AD_STATUS_ERR_SD = -1,
    AD_STATUS_ERR_WIFI = -2,
    AD_STATUS_ERR_SERVER = -3,
};

static QueueHandle_t s_airdrop_cmd_queue = nullptr;
static TaskHandle_t s_airdrop_worker_task = nullptr;
static volatile int s_airdrop_worker_status = AD_STATUS_IDLE;

static void airdrop_worker_loop(void* arg) {
    AirDropCmd cmd;
    while (true) {
        if (xQueueReceive(s_airdrop_cmd_queue, &cmd, portMAX_DELAY) == pdTRUE) {
            if (cmd == AIRDROP_CMD_START_AP) {
                s_airdrop_worker_status = AD_STATUS_STARTING_AP;
                ESP_LOGI(TAG, "[Worker] Starting AirDrop SoftAP + HTTP Server (BLE cleanly paused for 2.4GHz coex)...");
                app_tars_stop_all_audio();
                AirDropManager::stop_server();
                wifi_manager_stop();

                // Cleanly pause BLE radio while SoftAP Hotspot is running (zero 2.4GHz contention / zero disconnect storms)
                ble_manager_set_softap_pause(true);
                vTaskDelay(pdMS_TO_TICKS(100));

                if (AirDropManager::init() != ESP_OK) {
                    ESP_LOGE(TAG, "[Worker] SD Card init failed");
                    ble_manager_set_softap_pause(false);
                    s_airdrop_worker_status = AD_STATUS_ERR_SD;
                    continue;
                }
                if (AirDropManager::start_server() != ESP_OK) {
                    ESP_LOGE(TAG, "[Worker] HTTP Server start failed");
                    ble_manager_set_softap_pause(false);
                    s_airdrop_worker_status = AD_STATUS_ERR_SERVER;
                    continue;
                }

                esp_err_t w_err = wifi_manager_start_ap("UZ_WATCH_AirDrop", NULL);
                if (w_err != ESP_OK) {
                    ESP_LOGE(TAG, "[Worker] SoftAP start failed: %s", esp_err_to_name(w_err));
                    AirDropManager::stop_server();
                    ble_manager_set_softap_pause(false);
                    s_airdrop_worker_status = AD_STATUS_ERR_WIFI;
                    continue;
                }
                s_airdrop_worker_status = AD_STATUS_READY_AP;
                ESP_LOGI(TAG, "[Worker] AirDrop SoftAP + HTTP Server READY at http://192.168.4.1");

            } else if (cmd == AIRDROP_CMD_START_STA) {
                s_airdrop_worker_status = AD_STATUS_STARTING_STA;
                ESP_LOGI(TAG, "[Worker] Switching AirDrop to Home Wi-Fi (STA) + Resuming BLE...");
                app_tars_stop_all_audio();
                AirDropManager::stop_server();
                wifi_manager_stop();
                ble_manager_set_softap_pause(false);

                if (AirDropManager::init() != ESP_OK) {
                    s_airdrop_worker_status = AD_STATUS_ERR_SD;
                    continue;
                }
                if (AirDropManager::start_server() != ESP_OK) {
                    s_airdrop_worker_status = AD_STATUS_ERR_SERVER;
                    continue;
                }

                esp_err_t w_err = wifi_manager_connect_default();
                if (w_err != ESP_OK) {
                    ESP_LOGE(TAG, "[Worker] Home Wi-Fi start failed: %s", esp_err_to_name(w_err));
                    AirDropManager::stop_server();
                    s_airdrop_worker_status = AD_STATUS_ERR_WIFI;
                    continue;
                }
                s_airdrop_worker_status = AD_STATUS_READY_STA;

            } else if (cmd == AIRDROP_CMD_STOP_ALL) {
                s_airdrop_worker_status = AD_STATUS_STOPPING;
                ESP_LOGI(TAG, "[Worker] Stopping AirDrop HTTP Server & Wi-Fi -> Resuming BLE...");
                AirDropManager::stop_server();
                wifi_manager_stop();
                ble_manager_set_softap_pause(false);
                s_airdrop_worker_status = AD_STATUS_IDLE;
            }
        }
    }
}

static void dispatch_airdrop_cmd(AirDropCmd cmd) {
    if (s_airdrop_cmd_queue == nullptr) {
        s_airdrop_cmd_queue = xQueueCreate(1, sizeof(AirDropCmd));
    }
    if (s_airdrop_worker_task == nullptr && s_airdrop_cmd_queue != nullptr) {
        xTaskCreatePinnedToCoreWithCaps(
            airdrop_worker_loop,
            "airdrop_wrk",
            5120,
            nullptr,
            4,
            &s_airdrop_worker_task,
            0,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
        );
    }
    if (s_airdrop_cmd_queue != nullptr) {
        xQueueOverwrite(s_airdrop_cmd_queue, &cmd);
    }
}

void AppAirDrop::request_stop_async() {
    if (wifi_manager_is_ap_active()) {
        dispatch_airdrop_cmd(AIRDROP_CMD_STOP_ALL);
    }
}

static void on_airdrop_back_clicked(lv_event_t* e) {
    AppAirDrop* app = (AppAirDrop*)lv_event_get_user_data(e);
    if (app) {
        lv_async_call([](void* data) {
            AppAirDrop* a = (AppAirDrop*)data;
            if (a) {
                a->request_close();
            }
        }, app);
    }
}

static void on_airdrop_mode_toggle_clicked(lv_event_t* e) {
    AppAirDrop* app = (AppAirDrop*)lv_event_get_user_data(e);
    if (!app) return;

    display_manager_reset_timer();
    int st = s_airdrop_worker_status;
    if (st == AD_STATUS_STARTING_AP || st == AD_STATUS_STARTING_STA || st == AD_STATUS_STOPPING) {
        return; // Busy transitioning
    }

    app->_last_rendered_state = -999;
    if (wifi_manager_is_ap_active()) {
        if (app->_label_desc) lv_label_set_text(app->_label_desc, "Ev Wi-Fi agina geciliyor...\nLutfen bekleyin.");
        if (app->_label_btn_mode) lv_label_set_text(app->_label_btn_mode, LV_SYMBOL_WIFI " Hotspot Ac (Internetsiz)");
        dispatch_airdrop_cmd(AIRDROP_CMD_START_STA);
    } else {
        if (app->_label_desc) lv_label_set_text(app->_label_desc, "Hotspot aciliyor...\nLutfen bekleyin.");
        if (app->_label_btn_mode) lv_label_set_text(app->_label_btn_mode, LV_SYMBOL_HOME " Ev Wi-Fi'a Gec");
        dispatch_airdrop_cmd(AIRDROP_CMD_START_AP);
    }
}

AppAirDrop::AppAirDrop()
    : esp_brookesia::systems::phone::App("AirDrop", &icon_airdrop, true),
      _bg_obj(nullptr), _btn_back(nullptr), _label_status(nullptr), _label_desc(nullptr),
      _btn_toggle_mode(nullptr), _label_btn_mode(nullptr), _poll_timer(nullptr),
      _saved_timeout_ms(5000), _last_rendered_state(-999) {
}

AppAirDrop::~AppAirDrop() {
}

bool AppAirDrop::run() {
    if (_bg_obj != nullptr) return true;

    // Keep screen & SoftAP alive for 180s while user converts songs / crops wallpaper / transfers files
    uint32_t cur_tmo = display_manager_get_timeout();
    _saved_timeout_ms = (cur_tmo > 0 && cur_tmo < 60000) ? cur_tmo : 5000;
    display_manager_set_timeout(180000);
    _last_rendered_state = -999;

    lv_obj_t* scr = lv_scr_act();
    _bg_obj = lv_obj_create(scr);
    lv_obj_set_size(_bg_obj, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(_bg_obj, lv_color_hex(0x121212), 0);
    lv_obj_set_style_bg_opa(_bg_obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_bg_obj, 0, 0);
    lv_obj_set_style_radius(_bg_obj, 0, 0);
    lv_obj_set_style_shadow_width(_bg_obj, 0, 0);
    lv_obj_set_style_shadow_spread(_bg_obj, 0, 0);
    lv_obj_set_style_shadow_opa(_bg_obj, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(_bg_obj, LV_OBJ_FLAG_SCROLLABLE);

    // Top Header: Back Button (<)
    _btn_back = lv_obj_create(_bg_obj);
    lv_obj_add_flag(_btn_back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(_btn_back, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(_btn_back, 0, 0);
    lv_obj_set_size(_btn_back, 44, 44);
    lv_obj_align(_btn_back, LV_ALIGN_TOP_LEFT, 15, 15);
    lv_obj_set_style_radius(_btn_back, 22, 0);
    lv_obj_set_style_bg_color(_btn_back, lv_color_hex(0x222222), 0);
    lv_obj_set_style_bg_opa(_btn_back, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(_btn_back, lv_color_hex(0x444444), 0);
    lv_obj_set_style_border_width(_btn_back, 1, 0);
    lv_obj_set_style_shadow_width(_btn_back, 0, 0);
    lv_obj_set_style_shadow_spread(_btn_back, 0, 0);
    lv_obj_set_style_shadow_opa(_btn_back, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(_btn_back, on_airdrop_back_clicked, LV_EVENT_CLICKED, this);

    lv_obj_t* lbl_back = lv_label_create(_btn_back);
    lv_obj_set_style_text_font(lbl_back, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(lbl_back, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(lbl_back, LV_SYMBOL_LEFT);
    lv_obj_center(lbl_back);

    // Main Status Label
    _label_status = lv_label_create(_bg_obj);
    lv_obj_set_style_text_color(_label_status, lv_color_hex(0xFFA000), 0);
    lv_obj_set_style_text_font(_label_status, &lv_font_montserrat_22, 0);
    lv_label_set_text(_label_status, "AirDrop & Muzik");
    lv_obj_align(_label_status, LV_ALIGN_TOP_LEFT, 68, 22);

    // Description Label
    _label_desc = lv_label_create(_bg_obj);
    lv_obj_set_width(_label_desc, 360);
    lv_obj_set_style_text_color(_label_desc, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(_label_desc, &lv_font_montserrat_16, 0);
    lv_label_set_text(_label_desc, "Hotspot Baslatiliyor...\nLutfen 1-2 sn bekleyin.");
    lv_obj_set_style_text_align(_label_desc, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(_label_desc, LV_ALIGN_CENTER, 0, -15);

    // Mode Toggle Button (Hotspot / Home Wi-Fi)
    _btn_toggle_mode = lv_obj_create(_bg_obj);
    lv_obj_add_flag(_btn_toggle_mode, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(_btn_toggle_mode, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(_btn_toggle_mode, 0, 0);
    lv_obj_set_size(_btn_toggle_mode, 280, 46);
    lv_obj_align(_btn_toggle_mode, LV_ALIGN_BOTTOM_MID, 0, -25);
    lv_obj_set_style_radius(_btn_toggle_mode, 12, 0);
    lv_obj_set_style_bg_color(_btn_toggle_mode, lv_color_hex(0x222222), 0);
    lv_obj_set_style_bg_opa(_btn_toggle_mode, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(_btn_toggle_mode, lv_color_hex(0x00E676), 0);
    lv_obj_set_style_border_width(_btn_toggle_mode, 1, 0);
    lv_obj_set_style_shadow_width(_btn_toggle_mode, 0, 0);
    lv_obj_set_style_shadow_spread(_btn_toggle_mode, 0, 0);
    lv_obj_set_style_shadow_opa(_btn_toggle_mode, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(_btn_toggle_mode, on_airdrop_mode_toggle_clicked, LV_EVENT_CLICKED, this);

    _label_btn_mode = lv_label_create(_btn_toggle_mode);
    lv_obj_set_style_text_font(_label_btn_mode, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_label_btn_mode, lv_color_hex(0x00E676), 0);
    lv_label_set_text(_label_btn_mode, LV_SYMBOL_HOME " Ev Wi-Fi'a Gec");
    lv_obj_center(_label_btn_mode);

    // Start SoftAP + HTTP Server asynchronously on dedicated worker task (never blocks taskLVGL!)
    dispatch_airdrop_cmd(AIRDROP_CMD_START_AP);

    // Periodic Poll Timer for Wi-Fi / Server status updates (500ms)
    _poll_timer = lv_timer_create([](lv_timer_t* t) {
        AppAirDrop* app = (AppAirDrop*)t->user_data;
        if (!app || !app->_label_desc) return;

        int st = s_airdrop_worker_status;
        if (st == AD_STATUS_STARTING_AP) {
            if (app->_last_rendered_state != 10) {
                app->_last_rendered_state = 10;
                lv_label_set_text(app->_label_desc, "Hotspot Baslatiliyor...\nLutfen bekleyin.");
                if (app->_label_status) lv_obj_set_style_text_color(app->_label_status, lv_color_hex(0xFFA000), 0);
            }
            return;
        }
        if (st == AD_STATUS_STARTING_STA) {
            if (app->_last_rendered_state != 11) {
                app->_last_rendered_state = 11;
                lv_label_set_text(app->_label_desc, "Ev Wi-Fi Baglaniyor...\nLutfen bekleyin.");
                if (app->_label_status) lv_obj_set_style_text_color(app->_label_status, lv_color_hex(0xFFA000), 0);
            }
            return;
        }
        if (st == AD_STATUS_ERR_SD) {
            if (app->_last_rendered_state != -1) {
                app->_last_rendered_state = -1;
                lv_label_set_text(app->_label_desc, "SD Kart Okunamadi!\nLutfen SD karti kontrol edin.");
                if (app->_label_status) lv_obj_set_style_text_color(app->_label_status, lv_color_hex(0xFF5252), 0);
            }
            return;
        }
        if (st == AD_STATUS_ERR_WIFI || st == AD_STATUS_ERR_SERVER) {
            if (app->_last_rendered_state != -2) {
                app->_last_rendered_state = -2;
                lv_label_set_text(app->_label_desc, "Sunucu Baslatilamadi!\nAlttan tekrar deneyin.");
                if (app->_label_status) lv_obj_set_style_text_color(app->_label_status, lv_color_hex(0xFF5252), 0);
                if (app->_label_btn_mode) lv_label_set_text(app->_label_btn_mode, LV_SYMBOL_REFRESH " Yeniden Baslat");
            }
            return;
        }

        if (wifi_manager_is_ap_active() && st == AD_STATUS_READY_AP) {
            if (app->_last_rendered_state != 20) {
                app->_last_rendered_state = 20;
                lv_label_set_text(app->_label_desc,
                    "Hotspot Hazir!\n\n1. Telefonda Wi-Fi secin:\nUZ_WATCH_AirDrop\n\n2. Uygulamadan Gonderin\nIP: 192.168.4.1");
                if (app->_label_status) lv_obj_set_style_text_color(app->_label_status, lv_color_hex(0x00E676), 0);
                if (app->_label_btn_mode) lv_label_set_text(app->_label_btn_mode, LV_SYMBOL_HOME " Ev Wi-Fi'a Gec");
            }
            return;
        }

        char ip_str[32] = {0};
        wifi_manager_get_ip(ip_str);
        if (strlen(ip_str) > 0 && strcmp(ip_str, "0.0.0.0") != 0) {
            char display_text[160];
            snprintf(display_text, sizeof(display_text),
                "Ev Wi-Fi Hazir!\n\nUygulamaya bu IP'yi girin:\n%s\n\nTarayici: http://%s", ip_str, ip_str);
            lv_label_set_text(app->_label_desc, display_text);
            app->_last_rendered_state = 21;
            if (app->_label_status) lv_obj_set_style_text_color(app->_label_status, lv_color_hex(0x00E676), 0);
            if (app->_label_btn_mode) lv_label_set_text(app->_label_btn_mode, LV_SYMBOL_WIFI " Hotspot Ac (Internetsiz)");
        } else if (wifi_manager_is_active()) {
            if (app->_last_rendered_state != 22) {
                app->_last_rendered_state = 22;
                lv_label_set_text(app->_label_desc, "Wi-Fi Baglaniyor...\nLutfen bekleyin.");
                if (app->_label_status) lv_obj_set_style_text_color(app->_label_status, lv_color_hex(0xFFA000), 0);
            }
        } else {
            if (app->_last_rendered_state != 23) {
                app->_last_rendered_state = 23;
                lv_label_set_text(app->_label_desc, "Uyku tasarrufu: Wi-Fi kapandi.\nAsagidan tekrar acabilirsiniz.");
                if (app->_label_status) lv_obj_set_style_text_color(app->_label_status, lv_color_hex(0x9E9E9E), 0);
                if (app->_label_btn_mode) lv_label_set_text(app->_label_btn_mode, LV_SYMBOL_WIFI " Hotspot Yeniden Ac");
            }
        }
    }, 500, this);

    return true;
}

bool AppAirDrop::back() {
    return notifyCoreClosed();
}

bool AppAirDrop::close() {
    ESP_LOGI(TAG, "AirDrop Uygulamasi kapaniyor -> sunucu ve Wi-Fi arka planda durduruluyor.");
    if (_poll_timer) {
        lv_timer_del(_poll_timer);
        _poll_timer = nullptr;
    }

    display_manager_set_timeout(_saved_timeout_ms);
    dispatch_airdrop_cmd(AIRDROP_CMD_STOP_ALL);

    if (_bg_obj != nullptr) {
        lv_obj_delete_async(_bg_obj);
        _bg_obj = nullptr;
    }
    _btn_back = nullptr;
    _label_status = nullptr;
    _label_desc = nullptr;
    _btn_toggle_mode = nullptr;
    _label_btn_mode = nullptr;
    return true;
}
