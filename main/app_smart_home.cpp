#include "app_smart_home.hpp"
extern const lv_image_dsc_t icon_smarthome;
#include "esp_lib_utils.h"

extern "C" {
#include "ble_manager.h"
#include "wifi_manager.h"
}

using namespace esp_brookesia;

struct SmartBtnData {
    const char* cmd;
    const char* name;
    lv_color_t color;
};

static SmartBtnData s_btn_data[] = {
    {"SMART_HOME|LIGHT_1_TOGGLE", "Tavan Isigi", lv_color_hex(0xFFA000)}, // Amber
    {"SMART_HOME|PC_SLEEP",        "PC Uyut",      lv_color_hex(0xE53935)}  // Red
};

AppSmartHome::AppSmartHome() : App("Akilli Ev", &icon_smarthome, true) {
    _bg_obj = nullptr;
    _wifi_status_label = nullptr;
    _wifi_ip_label = nullptr;
    _wifi_btn = nullptr;
    _wifi_btn_label = nullptr;
    _feedback_label = nullptr;
    _timer = nullptr;
}

AppSmartHome::~AppSmartHome() {
}

void AppSmartHome::send_command(const char* cmd, const char* friendly_name) {
    ESP_UTILS_LOGI("SmartHome Command: %s (%s)", cmd, friendly_name);
    
    // 1. Send via BLE to Companion App (bridge to PC / Home Assistant)
    ble_manager_send_media_command(cmd);

    // 2. Update visual feedback banner
    if (_feedback_label) {
        lv_label_set_text_fmt(_feedback_label, "%s Gonderildi!", friendly_name);
        lv_obj_set_style_text_color(_feedback_label, lv_color_hex(0x1DB954), 0); // Green
    }
}

void AppSmartHome::update_wifi_ui() {
    if (!_wifi_status_label || !_wifi_ip_label || !_wifi_btn_label) {
        return;
    }

    bool active = wifi_manager_is_active();
    bool connected = wifi_manager_is_connected();
    wifi_state_t state = wifi_manager_get_state();

    if (connected) {
        char ip[32] = {0};
        wifi_manager_get_ip(ip);
        lv_label_set_text(_wifi_status_label, LV_SYMBOL_WIFI " TurkTelekom");
        lv_obj_set_style_text_color(_wifi_status_label, lv_color_hex(0x4CAF50), 0); // Green
        lv_label_set_text_fmt(_wifi_ip_label, "IP: %s (Bagli)", strlen(ip) > 0 ? ip : "Hazir");
        lv_obj_set_style_text_color(_wifi_ip_label, lv_color_hex(0xCCCCCC), 0);
        lv_label_set_text(_wifi_btn_label, "Kapat");
        if (_wifi_btn) lv_obj_set_style_bg_color(_wifi_btn, lv_color_hex(0xD32F2F), 0); // Red
    } else if (state == WIFI_STATE_CONNECTING) {
        lv_label_set_text(_wifi_status_label, LV_SYMBOL_REFRESH " Baglaniyor...");
        lv_obj_set_style_text_color(_wifi_status_label, lv_color_hex(0xFFA000), 0); // Amber
        lv_label_set_text(_wifi_ip_label, "TurkTelekom_ZYA41B...");
        lv_obj_set_style_text_color(_wifi_ip_label, lv_color_hex(0x9E9E9E), 0);
        lv_label_set_text(_wifi_btn_label, "Kapat");
        if (_wifi_btn) lv_obj_set_style_bg_color(_wifi_btn, lv_color_hex(0x424242), 0); // Dark Gray
    } else if (state == WIFI_STATE_FAILED) {
        lv_label_set_text(_wifi_status_label, LV_SYMBOL_CLOSE " Baglanamadi!");
        lv_obj_set_style_text_color(_wifi_status_label, lv_color_hex(0xFF5252), 0); // Red
        lv_label_set_text(_wifi_ip_label, "Aga ulasilamadi. Tekrar dene.");
        lv_obj_set_style_text_color(_wifi_ip_label, lv_color_hex(0xE57373), 0);
        lv_label_set_text(_wifi_btn_label, "Tekrar");
        if (_wifi_btn) lv_obj_set_style_bg_color(_wifi_btn, lv_color_hex(0xFF9800), 0); // Orange / Retry
    } else if (active) {
        lv_label_set_text(_wifi_status_label, LV_SYMBOL_WIFI " Wi-Fi: Acik");
        lv_obj_set_style_text_color(_wifi_status_label, lv_color_hex(0x00E5FF), 0); // Vibrant Cyan
        lv_label_set_text(_wifi_ip_label, "Baglanmak icin dokunun");
        lv_obj_set_style_text_color(_wifi_ip_label, lv_color_hex(0x9E9E9E), 0);
        lv_label_set_text(_wifi_btn_label, "Baglan");
        if (_wifi_btn) lv_obj_set_style_bg_color(_wifi_btn, lv_color_hex(0x1976D2), 0); // Blue
    } else {
        lv_label_set_text(_wifi_status_label, LV_SYMBOL_WARNING " Wi-Fi: Kapali");
        lv_obj_set_style_text_color(_wifi_status_label, lv_color_hex(0x888888), 0); // Gray
        lv_label_set_text(_wifi_ip_label, "Otomatik baglanmak icin acin");
        lv_obj_set_style_text_color(_wifi_ip_label, lv_color_hex(0x9E9E9E), 0);
        lv_label_set_text(_wifi_btn_label, "Ac");
        if (_wifi_btn) lv_obj_set_style_bg_color(_wifi_btn, lv_color_hex(0x1976D2), 0); // Blue
    }
}

void AppSmartHome::on_wifi_toggle_clicked(lv_event_t* e) {
    AppSmartHome* app = (AppSmartHome*)lv_event_get_user_data(e);
    if (!app) return;

    if (!wifi_manager_is_active()) {
        ESP_UTILS_LOGI("Starting Wi-Fi and auto-connecting to TurkTelekom");
        wifi_manager_start();
    } else if (wifi_manager_get_state() == WIFI_STATE_FAILED) {
        ESP_UTILS_LOGI("Retrying connection to TurkTelekom");
        wifi_manager_connect_default();
    } else if (wifi_manager_get_state() == WIFI_STATE_IDLE) {
        ESP_UTILS_LOGI("Connecting to TurkTelekom from IDLE");
        wifi_manager_connect_default();
    } else {
        ESP_UTILS_LOGI("Stopping Wi-Fi from SmartHome UI");
        wifi_manager_stop();
    }

    app->update_wifi_ui();
}

void AppSmartHome::on_timer_tick(lv_timer_t* t) {
    AppSmartHome* app = (AppSmartHome*)lv_timer_get_user_data(t);
    if (app) {
        app->update_wifi_ui();
    }
}

void AppSmartHome::on_btn_clicked(lv_event_t* e) {
    AppSmartHome* app = (AppSmartHome*)lv_event_get_user_data(e);
    lv_obj_t* btn = (lv_obj_t*)lv_event_get_target(e);
    SmartBtnData* data = (SmartBtnData*)lv_obj_get_user_data(btn);

    if (app && data) {
        // Visual feedback flash
        lv_obj_set_style_bg_color(btn, data->color, 0);

        app->send_command(data->cmd, data->name);
    }
}

bool AppSmartHome::run() {
    ESP_UTILS_LOGI("AppSmartHome run - 2 Large Buttons & Clean Wi-Fi UI");
    if (_bg_obj != nullptr) {
        return true;
    }

    _bg_obj = lv_obj_create(lv_scr_act());
    lv_obj_set_size(_bg_obj, LV_PCT(100), LV_PCT(100));
    lv_obj_center(_bg_obj);
    lv_obj_set_style_bg_color(_bg_obj, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(_bg_obj, 0, 0);
    lv_obj_set_style_radius(_bg_obj, 0, 0);
    lv_obj_clear_flag(_bg_obj, LV_OBJ_FLAG_SCROLLABLE);

    // 1. Top Title
    lv_obj_t* title = lv_label_create(_bg_obj);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(title, "AKILLI EV & WI-FI");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 15);

    // 2. Wi-Fi Status & Control Card (Height 80, Width 370)
    lv_obj_t* wifi_card = lv_obj_create(_bg_obj);
    lv_obj_set_size(wifi_card, 370, 80);
    lv_obj_align(wifi_card, LV_ALIGN_TOP_MID, 0, 48);
    lv_obj_set_style_bg_color(wifi_card, lv_color_hex(0x161616), 0);
    lv_obj_set_style_border_color(wifi_card, lv_color_hex(0x2C2C2C), 0);
    lv_obj_set_style_border_width(wifi_card, 1, 0);
    lv_obj_set_style_radius(wifi_card, 16, 0);
    lv_obj_clear_flag(wifi_card, LV_OBJ_FLAG_SCROLLABLE);

    _wifi_status_label = lv_label_create(wifi_card);
    lv_obj_set_style_text_font(_wifi_status_label, &lv_font_montserrat_18, 0);
    lv_obj_align(_wifi_status_label, LV_ALIGN_LEFT_MID, 5, -12);

    _wifi_ip_label = lv_label_create(wifi_card);
    lv_obj_set_style_text_font(_wifi_ip_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_wifi_ip_label, lv_color_hex(0x9E9E9E), 0);
    lv_obj_align(_wifi_ip_label, LV_ALIGN_LEFT_MID, 5, 12);

    _wifi_btn = lv_btn_create(wifi_card);
    lv_obj_set_size(_wifi_btn, 85, 48);
    lv_obj_align(_wifi_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_radius(_wifi_btn, 12, 0);
    lv_obj_add_event_cb(_wifi_btn, on_wifi_toggle_clicked, LV_EVENT_CLICKED, this);

    _wifi_btn_label = lv_label_create(_wifi_btn);
    lv_obj_set_style_text_font(_wifi_btn_label, &lv_font_montserrat_16, 0);
    lv_obj_center(_wifi_btn_label);

    // Initial Wi-Fi status update
    update_wifi_ui();

    // 3. Feedback Banner
    _feedback_label = lv_label_create(_bg_obj);
    lv_obj_set_style_text_font(_feedback_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_feedback_label, lv_color_hex(0x757575), 0);
    lv_label_set_text(_feedback_label, "Komut gondermek icin dokunun");
    lv_obj_align(_feedback_label, LV_ALIGN_TOP_MID, 0, 138);

    // 4. Button 1: Tavan Isigi Card (Height 125, Width 370, Y = 170)
    lv_obj_t* btn1 = lv_btn_create(_bg_obj);
    lv_obj_set_size(btn1, 370, 125);
    lv_obj_align(btn1, LV_ALIGN_TOP_MID, 0, 170);
    lv_obj_set_style_bg_color(btn1, lv_color_hex(0x181818), 0);
    lv_obj_set_style_border_color(btn1, lv_color_hex(0x333333), 0);
    lv_obj_set_style_border_width(btn1, 1, 0);
    lv_obj_set_style_radius(btn1, 20, 0);
    lv_obj_set_user_data(btn1, &s_btn_data[0]);
    lv_obj_add_event_cb(btn1, on_btn_clicked, LV_EVENT_CLICKED, this);

    // Icon Circle Badge
    lv_obj_t* icon_bg1 = lv_obj_create(btn1);
    lv_obj_set_size(icon_bg1, 64, 64);
    lv_obj_align(icon_bg1, LV_ALIGN_LEFT_MID, 5, 0);
    lv_obj_set_style_radius(icon_bg1, 32, 0);
    lv_obj_set_style_bg_color(icon_bg1, lv_color_hex(0x332200), 0);
    lv_obj_set_style_border_width(icon_bg1, 0, 0);
    lv_obj_clear_flag(icon_bg1, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* icon1 = lv_label_create(icon_bg1);
    lv_obj_set_style_text_font(icon1, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(icon1, lv_color_hex(0xFFA000), 0);
    lv_label_set_text(icon1, LV_SYMBOL_IMAGE);
    lv_obj_center(icon1);

    // Title & Subtitle
    lv_obj_t* title1 = lv_label_create(btn1);
    lv_obj_set_style_text_font(title1, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title1, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(title1, "Tavan Isigi");
    lv_obj_align(title1, LV_ALIGN_LEFT_MID, 85, -14);

    lv_obj_t* sub1 = lv_label_create(btn1);
    lv_obj_set_style_text_font(sub1, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(sub1, lv_color_hex(0x9E9E9E), 0);
    lv_label_set_text(sub1, "Dokun: Ac / Kapat");
    lv_obj_align(sub1, LV_ALIGN_LEFT_MID, 85, 14);

    // Right Arrow
    lv_obj_t* right_arrow1 = lv_label_create(btn1);
    lv_obj_set_style_text_font(right_arrow1, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(right_arrow1, lv_color_hex(0x666666), 0);
    lv_label_set_text(right_arrow1, LV_SYMBOL_RIGHT);
    lv_obj_align(right_arrow1, LV_ALIGN_RIGHT_MID, -5, 0);

    // 5. Button 2: PC Uyut Card (Height 125, Width 370, Y = 310)
    lv_obj_t* btn2 = lv_btn_create(_bg_obj);
    lv_obj_set_size(btn2, 370, 125);
    lv_obj_align(btn2, LV_ALIGN_TOP_MID, 0, 310);
    lv_obj_set_style_bg_color(btn2, lv_color_hex(0x181818), 0);
    lv_obj_set_style_border_color(btn2, lv_color_hex(0x333333), 0);
    lv_obj_set_style_border_width(btn2, 1, 0);
    lv_obj_set_style_radius(btn2, 20, 0);
    lv_obj_set_user_data(btn2, &s_btn_data[1]);
    lv_obj_add_event_cb(btn2, on_btn_clicked, LV_EVENT_CLICKED, this);

    // Icon Circle Badge
    lv_obj_t* icon_bg2 = lv_obj_create(btn2);
    lv_obj_set_size(icon_bg2, 64, 64);
    lv_obj_align(icon_bg2, LV_ALIGN_LEFT_MID, 5, 0);
    lv_obj_set_style_radius(icon_bg2, 32, 0);
    lv_obj_set_style_bg_color(icon_bg2, lv_color_hex(0x331010), 0);
    lv_obj_set_style_border_width(icon_bg2, 0, 0);
    lv_obj_clear_flag(icon_bg2, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* icon2 = lv_label_create(icon_bg2);
    lv_obj_set_style_text_font(icon2, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(icon2, lv_color_hex(0xE53935), 0);
    lv_label_set_text(icon2, LV_SYMBOL_POWER);
    lv_obj_center(icon2);

    // Title & Subtitle
    lv_obj_t* title2 = lv_label_create(btn2);
    lv_obj_set_style_text_font(title2, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title2, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(title2, "PC Uyut");
    lv_obj_align(title2, LV_ALIGN_LEFT_MID, 85, -14);

    lv_obj_t* sub2 = lv_label_create(btn2);
    lv_obj_set_style_text_font(sub2, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(sub2, lv_color_hex(0x9E9E9E), 0);
    lv_label_set_text(sub2, "Dokun: Uyku Modu");
    lv_obj_align(sub2, LV_ALIGN_LEFT_MID, 85, 14);

    // Right Arrow
    lv_obj_t* right_arrow2 = lv_label_create(btn2);
    lv_obj_set_style_text_font(right_arrow2, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(right_arrow2, lv_color_hex(0x666666), 0);
    lv_label_set_text(right_arrow2, LV_SYMBOL_RIGHT);
    lv_obj_align(right_arrow2, LV_ALIGN_RIGHT_MID, -5, 0);

    // 6. Timer: Update Wi-Fi status every 1 second
    _timer = lv_timer_create(on_timer_tick, 1000, this);

    return true;
}

bool AppSmartHome::back() {
    return notifyCoreClosed();
}

bool AppSmartHome::close() {
    ESP_UTILS_LOGI("AppSmartHome close");
    if (_timer) {
        lv_timer_delete(_timer);
        _timer = nullptr;
    }
    if (_bg_obj) {
        lv_obj_delete(_bg_obj);
        _bg_obj = nullptr;
        _wifi_status_label = nullptr;
        _wifi_ip_label = nullptr;
        _wifi_btn = nullptr;
        _wifi_btn_label = nullptr;
        _feedback_label = nullptr;
    }
    return true;
}

