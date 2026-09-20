#include "lvgl.h"
extern "C" const lv_image_dsc_t icon_airdrop;


#include "app_airdrop.hpp"
#include "airdrop_manager.hpp"
#include "esp_log.h"

#include "wifi_manager.h"

static const char *TAG = "AppAirDrop";

static void on_airdrop_back_clicked(lv_event_t* e) {
    AppAirDrop* app = (AppAirDrop*)lv_event_get_user_data(e);
    if (app) {
        app->request_close();
    }
}

static void on_airdrop_mode_toggle_clicked(lv_event_t* e) {
    AppAirDrop* app = (AppAirDrop*)lv_event_get_user_data(e);
    if (!app) return;
    
    if (wifi_manager_is_ap_active()) {
        ESP_LOGI(TAG, "Switching to Default Wi-Fi (STA)...");
        wifi_manager_connect_default();
        if (app->_label_btn_mode) lv_label_set_text(app->_label_btn_mode, LV_SYMBOL_WIFI " Hotspot Ac (Internetsiz)");
        if (app->_label_desc) lv_label_set_text(app->_label_desc, "Wi-Fi Baglaniyor...\nLutfen bekleyin.");
    } else {
        ESP_LOGI(TAG, "Switching to Hotspot (SoftAP)...");
        wifi_manager_start_ap("UZ_WATCH_AirDrop", NULL);
        if (app->_label_btn_mode) lv_label_set_text(app->_label_btn_mode, LV_SYMBOL_HOME " Ev Wi-Fi'a Gec");
        if (app->_label_desc) {
            lv_label_set_text(app->_label_desc, "Hotspot Aktif (Internetsiz)!\n\n1. Telefonda baglanin:\nSSID: UZ_WATCH_AirDrop\n\n2. Tarayicida acin:\nhttp://192.168.4.1");
        }
    }
}

AppAirDrop::AppAirDrop() 
    : esp_brookesia::systems::phone::App("AirDrop", &icon_airdrop, true),
      _bg_obj(nullptr), _btn_back(nullptr), _label_status(nullptr), _label_desc(nullptr),
      _btn_toggle_mode(nullptr), _label_btn_mode(nullptr), _poll_timer(nullptr) {
}

AppAirDrop::~AppAirDrop() {
}

bool AppAirDrop::run() {
    lv_obj_t* scr = lv_scr_act();
    _bg_obj = lv_obj_create(scr);
    lv_obj_set_size(_bg_obj, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(_bg_obj, lv_color_hex(0x121212), 0);
    lv_obj_set_style_bg_opa(_bg_obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_bg_obj, 0, 0);
    lv_obj_set_style_radius(_bg_obj, 0, 0);

    // Top Header: Back Button (<)
    _btn_back = lv_btn_create(_bg_obj);
    lv_obj_set_size(_btn_back, 44, 44);
    lv_obj_align(_btn_back, LV_ALIGN_TOP_LEFT, 15, 15);
    lv_obj_set_style_radius(_btn_back, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(_btn_back, lv_color_hex(0x222222), 0);
    lv_obj_set_style_border_color(_btn_back, lv_color_hex(0x444444), 0);
    lv_obj_set_style_border_width(_btn_back, 1, 0);
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
    lv_label_set_text(_label_desc, "Wi-Fi Baslatiliyor...\nLutfen bekleyin.");
    lv_obj_set_style_text_align(_label_desc, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(_label_desc, LV_ALIGN_CENTER, 0, -20);

    // Mode Toggle Button (Hotspot / Home Wi-Fi)
    _btn_toggle_mode = lv_btn_create(_bg_obj);
    lv_obj_set_size(_btn_toggle_mode, 270, 46);
    lv_obj_align(_btn_toggle_mode, LV_ALIGN_BOTTOM_MID, 0, -25);
    lv_obj_set_style_radius(_btn_toggle_mode, 12, 0);
    lv_obj_set_style_bg_color(_btn_toggle_mode, lv_color_hex(0x222222), 0);
    lv_obj_set_style_border_color(_btn_toggle_mode, lv_color_hex(0x00E676), 0);
    lv_obj_set_style_border_width(_btn_toggle_mode, 1, 0);
    lv_obj_add_event_cb(_btn_toggle_mode, on_airdrop_mode_toggle_clicked, LV_EVENT_CLICKED, this);

    _label_btn_mode = lv_label_create(_btn_toggle_mode);
    lv_obj_set_style_text_font(_label_btn_mode, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_label_btn_mode, lv_color_hex(0x00E676), 0);
    lv_label_set_text(_label_btn_mode, LV_SYMBOL_WIFI " Hotspot Ac (Internetsiz)");
    lv_obj_center(_label_btn_mode);

    // Auto-connect Wi-Fi if not connected
    if (!wifi_manager_is_active()) {
        ESP_LOGI(TAG, "AirDrop started -> initiating Wi-Fi connection...");
        wifi_manager_connect_default();
    }

    // Initialize SD Card & start HTTP Server
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
            char display_text[160];
            snprintf(display_text, sizeof(display_text),
                "Hotspot Aktif (Internetsiz)!\n\n1. Telefonda baglanin:\nSSID: UZ_WATCH_AirDrop\n\n2. Tarayicida acin:\nhttp://192.168.4.1");
            lv_label_set_text(app->_label_desc, display_text);
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
            lv_label_set_text(app->_label_desc, "Wi-Fi Kapali!\nAsagidan Hotspot acabilirsiniz.");
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
    AirDropManager::stop_server();
    // Stop Wi-Fi to preserve battery
    wifi_manager_stop();

    if (_bg_obj != nullptr) {
        lv_obj_del(_bg_obj);
        _bg_obj = nullptr;
    }
    _btn_back = nullptr;
    _label_status = nullptr;
    _label_desc = nullptr;
    _btn_toggle_mode = nullptr;
    _label_btn_mode = nullptr;
    return true;
}







