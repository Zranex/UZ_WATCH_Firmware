#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
extern "C" const lv_image_dsc_t icon_airdrop;

#include "app_airdrop.hpp"
#include "airdrop_manager.hpp"
#include "esp_log.h"
#include "wifi_manager.h"
#include "display_manager.h"

static const char *TAG = "AppAirDrop";

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

    // Stop server FIRST to avoid LwIP socket use-after-free when switching interfaces
    AirDropManager::stop_server();

    if (wifi_manager_is_ap_active()) {
        ESP_LOGI(TAG, "Switching to Default Wi-Fi (STA)...");
        wifi_manager_connect_default();
        AirDropManager::start_server();
        if (app->_label_btn_mode) lv_label_set_text(app->_label_btn_mode, LV_SYMBOL_WIFI " Hotspot Ac (Internetsiz)");
        if (app->_label_desc) lv_label_set_text(app->_label_desc, "Wi-Fi Baglaniyor...\nLutfen bekleyin.");
    } else {
        ESP_LOGI(TAG, "Switching to Hotspot (SoftAP)...");
        wifi_manager_start_ap("UZ_WATCH_AirDrop", NULL);
        AirDropManager::start_server();
        if (app->_label_btn_mode) lv_label_set_text(app->_label_btn_mode, LV_SYMBOL_HOME " Ev Wi-Fi'a Gec");
        if (app->_label_desc) {
            lv_label_set_text(app->_label_desc, "Hotspot Aktif (Internetsiz)!\n\n1. Telefonda baglanin:\nSSID: UZ_WATCH_AirDrop\n\n2. Tarayicida acin:\nhttp://192.168.4.1");
        }
    }
}

AppAirDrop::AppAirDrop() 
    : esp_brookesia::systems::phone::App("AirDrop", &icon_airdrop, true),
      _bg_obj(nullptr), _btn_back(nullptr), _label_status(nullptr), _label_desc(nullptr),
      _btn_toggle_mode(nullptr), _label_btn_mode(nullptr), _poll_timer(nullptr),
      _saved_timeout_ms(5000) {
}

AppAirDrop::~AppAirDrop() {
}

bool AppAirDrop::run() {
    if (_bg_obj != nullptr) return true;

    // Keep screen & SoftAP alive for 90s while user connects phone & transfers files
    uint32_t cur_tmo = display_manager_get_timeout();
    _saved_timeout_ms = (cur_tmo > 0 && cur_tmo < 60000) ? cur_tmo : 5000;
    display_manager_set_timeout(90000);

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

    // Top Header: Back Button (<) (lv_obj_create to avoid lv_btn theme shadows)
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
    lv_obj_set_style_text_color(_label_status, lv_color_hex(0x00E676), 0);
    lv_obj_set_style_text_font(_label_status, &lv_font_montserrat_22, 0);
    lv_label_set_text(_label_status, "AirDrop & Medya");
    lv_obj_align(_label_status, LV_ALIGN_TOP_LEFT, 68, 22);

    // Description Label
    _label_desc = lv_label_create(_bg_obj);
    lv_obj_set_style_text_color(_label_desc, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(_label_desc, &lv_font_montserrat_16, 0);
    lv_label_set_text(_label_desc, "Hotspot Aktif (Internetsiz)!\n\n1. Telefonda baglanin:\nSSID: UZ_WATCH_AirDrop\n\n2. Tarayicida acin:\nhttp://192.168.4.1");
    lv_obj_set_style_text_align(_label_desc, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(_label_desc, LV_ALIGN_CENTER, 0, -20);

    // Mode Toggle Button (Hotspot / Home Wi-Fi)
    _btn_toggle_mode = lv_obj_create(_bg_obj);
    lv_obj_add_flag(_btn_toggle_mode, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(_btn_toggle_mode, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(_btn_toggle_mode, 0, 0);
    lv_obj_set_size(_btn_toggle_mode, 270, 46);
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

    // Cleanly stop any prior server instance before configuring SoftAP
    AirDropManager::stop_server();

    if (!wifi_manager_is_active() || !wifi_manager_is_ap_active()) {
        ESP_LOGI(TAG, "AirDrop started -> launching standalone SoftAP Hotspot...");
        wifi_manager_start_ap("UZ_WATCH_AirDrop", NULL);
    }

    if (AirDropManager::init() == ESP_OK) {
        if (AirDropManager::start_server() == ESP_OK) {
            ESP_LOGI(TAG, "AirDrop HTTP Server is active.");
        } else {
            lv_label_set_text(_label_desc, "Sunucu Baslatilamadi!\nSD Kart kontrol edin.");
            lv_obj_set_style_text_color(_label_status, lv_color_hex(0xFF5252), 0);
        }
    } else {
        lv_label_set_text(_label_desc, "SD Kart Okunamadi!\nLutfen Kart Takin.");
        lv_obj_set_style_text_color(_label_status, lv_color_hex(0xFF5252), 0);
    }

    // Periodic Poll Timer for Wi-Fi IP update
    _poll_timer = lv_timer_create([](lv_timer_t* t) {
        AppAirDrop* app = (AppAirDrop*)t->user_data;
        if (!app || !app->_label_desc) return;

        if (wifi_manager_is_ap_active()) {
            lv_label_set_text(app->_label_desc,
                "Hotspot Aktif (Internetsiz)!\n\n1. Telefonda baglanin:\nSSID: UZ_WATCH_AirDrop\n\n2. Tarayicida acin:\nhttp://192.168.4.1");
            if (app->_label_status) lv_obj_set_style_text_color(app->_label_status, lv_color_hex(0x00E676), 0);
            if (app->_label_btn_mode) lv_label_set_text(app->_label_btn_mode, LV_SYMBOL_HOME " Ev Wi-Fi'a Gec");
            return;
        }

        char ip_str[32] = {0};
        wifi_manager_get_ip(ip_str);
        if (strlen(ip_str) > 0 && strcmp(ip_str, "0.0.0.0") != 0) {
            char display_text[128];
            snprintf(display_text, sizeof(display_text), "Wi-Fi Baglantisi Hazir!\n\nTarayiciya yazin:\nhttp://%s", ip_str);
            lv_label_set_text(app->_label_desc, display_text);
            if (app->_label_status) {
                lv_obj_set_style_text_color(app->_label_status, lv_color_hex(0x00E676), 0);
            }
            if (app->_label_btn_mode) lv_label_set_text(app->_label_btn_mode, LV_SYMBOL_WIFI " Hotspot Ac (Internetsiz)");
        } else if (wifi_manager_is_active()) {
            lv_label_set_text(app->_label_desc, "Wi-Fi Baglaniyor...\nLutfen bekleyin.");
        } else {
            lv_label_set_text(app->_label_desc, "Uyku tasarrufu: Wi-Fi kapandi.\nAsagidan tekrar acabilirsiniz.");
            if (app->_label_btn_mode) lv_label_set_text(app->_label_btn_mode, LV_SYMBOL_WIFI " Hotspot Yeniden Ac");
        }
    }, 1000, this);

    return true;
}

bool AppAirDrop::back() {
    return notifyCoreClosed();
}

bool AppAirDrop::close() {
    ESP_LOGI(TAG, "AirDrop Uygulamasi kapaniyor, sunucu ve Wi-Fi durduruluyor.");
    if (_poll_timer) {
        lv_timer_del(_poll_timer);
        _poll_timer = nullptr;
    }

    // Restore normal smartwatch display timeout (5s)
    display_manager_set_timeout(_saved_timeout_ms);

    AirDropManager::stop_server();
    wifi_manager_stop();

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
