#include "lvgl.h"
extern "C" const lv_image_dsc_t icon_airdrop;


#include "app_airdrop.hpp"
#include "airdrop_manager.hpp"
#include "esp_log.h"

static const char *TAG = "AppAirDrop";

AppAirDrop::AppAirDrop() 
    : esp_brookesia::systems::phone::App("AirDrop", &icon_airdrop, true),
      _label_status(nullptr), _label_desc(nullptr) {
}

AppAirDrop::~AppAirDrop() {
}

bool AppAirDrop::run() {
    lv_obj_t* scr = lv_scr_act();
    _bg_obj = lv_obj_create(scr);
    lv_obj_set_size(_bg_obj, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(_bg_obj, lv_color_hex(0x1a1a2e), 0);
    lv_obj_set_style_bg_opa(_bg_obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_bg_obj, 0, 0);
    lv_obj_set_style_radius(_bg_obj, 0, 0);

    // Main Status Label
    _label_status = lv_label_create(_bg_obj);
    lv_obj_set_style_text_color(_label_status, lv_color_hex(0x0f3460), 0);
    lv_obj_set_style_text_font(_label_status, &lv_font_montserrat_20, 0);
    lv_label_set_text(_label_status, "AirDrop Aktif");
    lv_obj_align(_label_status, LV_ALIGN_TOP_MID, 0, 80);

    // Description Label
    _label_desc = lv_label_create(_bg_obj);
    lv_obj_set_style_text_color(_label_desc, lv_color_hex(0xe94560), 0);
    lv_label_set_text(_label_desc, "Mini Bulut Sunucusu\nBaslatiliyor...");
    lv_obj_set_style_text_align(_label_desc, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(_label_desc, LV_ALIGN_CENTER, 0, 0);

    // Initialize SD Card
    if (AirDropManager::init() == ESP_OK) {
        if (AirDropManager::start_server() == ESP_OK) {
            lv_label_set_text(_label_desc, "Baglantiya Hazir!\nhttp://uzwatch.local");
            lv_obj_set_style_text_color(_label_status, lv_color_hex(0x00FF00), 0);
        } else {
            lv_label_set_text(_label_desc, "Sunucu Baslatilamadi.\nWiFi Bagli mi?");
            lv_obj_set_style_text_color(_label_status, lv_color_hex(0xFF0000), 0);
        }
    } else {
        lv_label_set_text(_label_desc, "SD Kart Okunamadi!\nLutfen Kart Takin.");
        lv_obj_set_style_text_color(_label_status, lv_color_hex(0xFF0000), 0);
    }

    return true;
}

bool AppAirDrop::back() {
    return true;
}

bool AppAirDrop::close() {
    ESP_LOGI(TAG, "AirDrop Uygulamasi kapaniyor, sunucu durduruluyor...");
    AirDropManager::stop_server();
    _bg_obj = nullptr;
    _label_status = nullptr;
    _label_desc = nullptr;
    return true;
}







