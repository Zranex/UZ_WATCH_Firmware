#include "app_power.hpp"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include "bsp/esp-bsp.h"
#include "bsp_board_extra.h"
#include "display_manager.h"

extern "C" {
void system_set_wrist_wake_enabled(bool enabled);
bool system_is_wrist_wake_enabled(void);
}

extern const lv_image_dsc_t icon_power;

#define TAG "AppPower"

AppPower* AppPower::_instance = nullptr;
PowerProfile AppPower::_active_profile = POWER_PROFILE_BALANCED;

static void on_power_back_clicked(lv_event_t* e) {
    AppPower* app = (AppPower*)lv_event_get_user_data(e);
    if (app) {
        lv_async_call([](void* data) {
            AppPower* a = (AppPower*)data;
            if (a) a->request_close();
        }, app);
    }
}

void AppPower::set_power_profile(PowerProfile profile) {
    _active_profile = profile;
    switch (profile) {
    case POWER_PROFILE_BALANCED:
        display_manager_set_brightness(50);
        display_manager_set_timeout(5000);
        system_set_wrist_wake_enabled(true);
        ESP_LOGI(TAG, "Profile set to BALANCED (50%% brightness, 5s timeout, wrist wake ON)");
        break;
    case POWER_PROFILE_SCHOOL:
        display_manager_set_brightness(40);
        display_manager_set_timeout(4000);
        system_set_wrist_wake_enabled(true);
        ESP_LOGI(TAG, "Profile set to SCHOOL (40%% brightness, 4s timeout, silent alerts)");
        break;
    case POWER_PROFILE_SUPER_SAVER:
        display_manager_set_brightness(25);
        display_manager_set_timeout(3000);
        system_set_wrist_wake_enabled(false);
        ESP_LOGI(TAG, "Profile set to SUPER SAVER (25%% brightness, 3s timeout, wrist wake OFF -> 2.5x battery)");
        break;
    }
    if (_instance) {
        _instance->update_ui();
    }
}

void AppPower::on_profile_clicked(lv_event_t* e) {
    int profile_val = (int)(intptr_t)lv_event_get_user_data(e);
    set_power_profile((PowerProfile)profile_val);
}

void AppPower::on_wrist_toggle_clicked(lv_event_t* e) {
    bool current = system_is_wrist_wake_enabled();
    system_set_wrist_wake_enabled(!current);
    if (_instance) _instance->update_ui();
}

void AppPower::on_sleep_now_clicked(lv_event_t* e) {
    display_manager_turn_off();
}

AppPower::AppPower()
    : esp_brookesia::systems::phone::App("Batarya", &icon_power, true),
      _bg_obj(nullptr), _btn_back(nullptr), _arc_battery(nullptr),
      _lbl_percent(nullptr), _lbl_voltage(nullptr), _lbl_charging(nullptr),
      _lbl_estimate(nullptr), _btn_balanced(nullptr), _btn_school(nullptr),
      _btn_saver(nullptr), _btn_wrist_toggle(nullptr), _lbl_wrist_toggle(nullptr),
      _btn_sleep_now(nullptr), _poll_timer(nullptr)
{
    _instance = this;
}

AppPower::~AppPower() {
    if (_instance == this) _instance = nullptr;
}

bool AppPower::init() {
    _instance = this;
    return true;
}

bool AppPower::back() {
    return close();
}

bool AppPower::close() {
    if (_poll_timer) {
        lv_timer_delete(_poll_timer);
        _poll_timer = nullptr;
    }
    if (_bg_obj) {
        lv_obj_delete_async(_bg_obj);
        _bg_obj = nullptr;
    }
    return true;
}

bool AppPower::run() {
    _bg_obj = lv_obj_create(lv_scr_act());
    lv_obj_set_size(_bg_obj, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(_bg_obj, lv_color_hex(0x0A0F1D), 0); // Sleek deep dark background
    lv_obj_set_style_bg_opa(_bg_obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_bg_obj, 0, 0);
    lv_obj_set_style_radius(_bg_obj, 0, 0);
    lv_obj_set_style_pad_all(_bg_obj, 0, 0);
    lv_obj_set_flex_flow(_bg_obj, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(_bg_obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(_bg_obj, LV_SCROLLBAR_MODE_AUTO);

    // 1. Header (Geri butonu + Baslik)
    lv_obj_t* header = lv_obj_create(_bg_obj);
    lv_obj_set_size(header, LV_PCT(100), 55);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_hor(header, 12, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    _btn_back = lv_btn_create(header);
    lv_obj_set_size(_btn_back, 75, 40);
    lv_obj_align(_btn_back, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(_btn_back, lv_color_hex(0x1F2937), 0);
    lv_obj_set_style_radius(_btn_back, 12, 0);
    lv_obj_set_style_shadow_width(_btn_back, 0, 0);
    lv_obj_add_event_cb(_btn_back, on_power_back_clicked, LV_EVENT_CLICKED, this);

    lv_obj_t* lbl_back = lv_label_create(_btn_back);
    lv_label_set_text(lbl_back, LV_SYMBOL_LEFT " Geri");
    lv_obj_set_style_text_color(lbl_back, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl_back, &lv_font_montserrat_14, 0);
    lv_obj_center(lbl_back);

    lv_obj_t* lbl_title = lv_label_create(header);
    lv_label_set_text(lbl_title, "Guc & Pil");
    lv_obj_set_style_text_color(lbl_title, lv_color_hex(0x10B981), 0); // Green accent
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_20, 0);
    lv_obj_align(lbl_title, LV_ALIGN_CENTER, 0, 0);

    // 2. Visual Battery Arc Card
    lv_obj_t* card_gauge = lv_obj_create(_bg_obj);
    lv_obj_set_size(card_gauge, 380, 210);
    lv_obj_set_style_bg_color(card_gauge, lv_color_hex(0x111827), 0);
    lv_obj_set_style_bg_opa(card_gauge, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card_gauge, lv_color_hex(0x1F2937), 0);
    lv_obj_set_style_border_width(card_gauge, 2, 0);
    lv_obj_set_style_radius(card_gauge, 20, 0);
    lv_obj_set_style_shadow_width(card_gauge, 0, 0);
    lv_obj_clear_flag(card_gauge, LV_OBJ_FLAG_SCROLLABLE);

    _arc_battery = lv_arc_create(card_gauge);
    lv_obj_set_size(_arc_battery, 170, 170);
    lv_arc_set_rotation(_arc_battery, 135);
    lv_arc_set_bg_angles(_arc_battery, 0, 270);
    lv_arc_set_range(_arc_battery, 0, 100);
    lv_obj_set_style_arc_width(_arc_battery, 14, LV_PART_MAIN);
    lv_obj_set_style_arc_color(_arc_battery, lv_color_hex(0x1F2937), LV_PART_MAIN);
    lv_obj_set_style_arc_width(_arc_battery, 14, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(_arc_battery, lv_color_hex(0x10B981), LV_PART_INDICATOR);
    lv_obj_remove_style(_arc_battery, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(_arc_battery, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(_arc_battery, LV_ALIGN_LEFT_MID, 5, 0);

    _lbl_percent = lv_label_create(_arc_battery);
    lv_obj_set_style_text_font(_lbl_percent, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(_lbl_percent, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(_lbl_percent, "100%");
    lv_obj_align(_lbl_percent, LV_ALIGN_CENTER, 0, -8);

    // Right Column Info inside Gauge Card
    lv_obj_t* info_col = lv_obj_create(card_gauge);
    lv_obj_set_size(info_col, 180, 170);
    lv_obj_align(info_col, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_opa(info_col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(info_col, 0, 0);
    lv_obj_set_style_pad_all(info_col, 5, 0);
    lv_obj_set_flex_flow(info_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(info_col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(info_col, LV_OBJ_FLAG_SCROLLABLE);

    _lbl_charging = lv_label_create(info_col);
    lv_obj_set_style_text_font(_lbl_charging, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_lbl_charging, lv_color_hex(0x10B981), 0);
    lv_label_set_text(_lbl_charging, "Pilde");

    _lbl_voltage = lv_label_create(info_col);
    lv_obj_set_style_text_font(_lbl_voltage, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(_lbl_voltage, lv_color_hex(0x60A5FA), 0);
    lv_label_set_text(_lbl_voltage, "4,150 mV");

    _lbl_estimate = lv_label_create(info_col);
    lv_obj_set_style_text_font(_lbl_estimate, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_lbl_estimate, lv_color_hex(0xE5E7EB), 0);
    lv_label_set_text(_lbl_estimate, "Kalan: ~36s");

    // 3. Power Profiles Header
    lv_obj_t* lbl_prof_title = lv_label_create(_bg_obj);
    lv_label_set_text(lbl_prof_title, "GUC PROFILLERI");
    lv_obj_set_style_text_font(lbl_prof_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_prof_title, lv_color_hex(0x9CA3AF), 0);
    lv_obj_set_style_pad_top(lbl_prof_title, 10, 0);
    lv_obj_set_style_pad_bottom(lbl_prof_title, 4, 0);

    // Profile Buttons Container
    auto create_profile_btn = [this](const char* title, const char* desc, PowerProfile prof) -> lv_obj_t* {
        lv_obj_t* btn = lv_obj_create(_bg_obj);
        lv_obj_set_size(btn, 380, 56);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x1F2937), 0);
        lv_obj_set_style_radius(btn, 14, 0);
        lv_obj_set_style_border_width(btn, 2, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(0x374151), 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_pad_hor(btn, 14, 0);
        lv_obj_set_style_pad_ver(btn, 6, 0);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(btn, on_profile_clicked, LV_EVENT_CLICKED, (void*)(intptr_t)prof);

        lv_obj_t* lbl_t = lv_label_create(btn);
        lv_label_set_text(lbl_t, title);
        lv_obj_set_style_text_font(lbl_t, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(lbl_t, lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(lbl_t, LV_ALIGN_TOP_LEFT, 0, 0);

        lv_obj_t* lbl_d = lv_label_create(btn);
        lv_label_set_text(lbl_d, desc);
        lv_obj_set_style_text_font(lbl_d, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(lbl_d, lv_color_hex(0x9CA3AF), 0);
        lv_obj_align(lbl_d, LV_ALIGN_BOTTOM_LEFT, 0, 0);

        return btn;
    };

    _btn_balanced = create_profile_btn("⚡ Dengeli Profil", "%50 Parlaklik | 5s Sure | Kol Kaldirma Acik", POWER_PROFILE_BALANCED);
    _btn_school = create_profile_btn("🏫 Okul Modu", "%40 Parlaklik | Sessiz Bildirim | 4s Sure", POWER_PROFILE_SCHOOL);
    _btn_saver = create_profile_btn("🌱 Super Tasarruf (2.5x Pil)", "%25 Parlaklik | 3s Sure | Kol Kaldirma Kapali", POWER_PROFILE_SUPER_SAVER);

    // 4. Quick Toggles Container
    lv_obj_t* lbl_quick_title = lv_label_create(_bg_obj);
    lv_label_set_text(lbl_quick_title, "HIZLI AYARLAR");
    lv_obj_set_style_text_font(lbl_quick_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_quick_title, lv_color_hex(0x9CA3AF), 0);
    lv_obj_set_style_pad_top(lbl_quick_title, 10, 0);
    lv_obj_set_style_pad_bottom(lbl_quick_title, 4, 0);

    _btn_wrist_toggle = lv_btn_create(_bg_obj);
    lv_obj_set_size(_btn_wrist_toggle, 380, 50);
    lv_obj_set_style_bg_color(_btn_wrist_toggle, lv_color_hex(0x1F2937), 0);
    lv_obj_set_style_radius(_btn_wrist_toggle, 14, 0);
    lv_obj_set_style_shadow_width(_btn_wrist_toggle, 0, 0);
    lv_obj_add_event_cb(_btn_wrist_toggle, on_wrist_toggle_clicked, LV_EVENT_CLICKED, this);

    _lbl_wrist_toggle = lv_label_create(_btn_wrist_toggle);
    lv_obj_set_style_text_font(_lbl_wrist_toggle, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_lbl_wrist_toggle, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(_lbl_wrist_toggle);

    _btn_sleep_now = lv_btn_create(_bg_obj);
    lv_obj_set_size(_btn_sleep_now, 380, 50);
    lv_obj_set_style_bg_color(_btn_sleep_now, lv_color_hex(0x374151), 0);
    lv_obj_set_style_radius(_btn_sleep_now, 14, 0);
    lv_obj_set_style_shadow_width(_btn_sleep_now, 0, 0);
    lv_obj_set_style_margin_bottom(_btn_sleep_now, 30, 0);
    lv_obj_add_event_cb(_btn_sleep_now, on_sleep_now_clicked, LV_EVENT_CLICKED, this);

    lv_obj_t* lbl_sleep = lv_label_create(_btn_sleep_now);
    lv_label_set_text(lbl_sleep, "🌙 Ekrani Kapat (Hemen Uyu)");
    lv_obj_set_style_text_font(lbl_sleep, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_sleep, lv_color_hex(0xE5E7EB), 0);
    lv_obj_center(lbl_sleep);

    update_ui();

    // Poll every 2 seconds
    _poll_timer = lv_timer_create([](lv_timer_t* t) {
        AppPower* app = (AppPower*)t->user_data;
        if (app) app->update_ui();
    }, 2000, this);

    return true;
}

void AppPower::update_ui() {
    if (!_bg_obj) return;

    int percent = bsp_battery_get_percent();
    int mv = bsp_battery_get_voltage_mv();
    bool charging = bsp_battery_is_charging();
    bool wrist_on = system_is_wrist_wake_enabled();

    if (_arc_battery) {
        lv_arc_set_value(_arc_battery, percent);
        // Dynamic arc color: Green (>50), Amber (20-50), Red (<20)
        lv_color_t arc_col = (percent > 50) ? lv_color_hex(0x10B981) :
                             (percent > 20) ? lv_color_hex(0xF59E0B) : lv_color_hex(0xEF4444);
        lv_obj_set_style_arc_color(_arc_battery, arc_col, LV_PART_INDICATOR);
    }

    if (_lbl_percent) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%%%d", percent);
        lv_label_set_text(_lbl_percent, buf);
    }

    if (_lbl_voltage) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%d mV", mv);
        lv_label_set_text(_lbl_voltage, buf);
    }

    if (_lbl_charging) {
        if (charging) {
            lv_label_set_text(_lbl_charging, "⚡ Sarj Ediliyor");
            lv_obj_set_style_text_color(_lbl_charging, lv_color_hex(0x34D399), 0);
        } else {
            lv_label_set_text(_lbl_charging, "🔋 Pilde");
            lv_obj_set_style_text_color(_lbl_charging, lv_color_hex(0x9CA3AF), 0);
        }
    }

    if (_lbl_estimate) {
        char buf[32];
        int est_hours = 0;
        if (_active_profile == POWER_PROFILE_SUPER_SAVER) {
            est_hours = (percent * 65) / 100;
        } else if (_active_profile == POWER_PROFILE_SCHOOL) {
            est_hours = (percent * 45) / 100;
        } else {
            est_hours = (percent * 35) / 100;
        }
        if (est_hours < 1) est_hours = 1;
        snprintf(buf, sizeof(buf), "Kalan: ~%d Saat", est_hours);
        lv_label_set_text(_lbl_estimate, buf);
    }

    // Highlight selected profile button
    auto update_btn_style = [this](lv_obj_t* btn, PowerProfile prof) {
        if (!btn) return;
        if (_active_profile == prof) {
            lv_obj_set_style_border_color(btn, lv_color_hex(0x10B981), 0);
            lv_obj_set_style_bg_color(btn, lv_color_hex(0x064E3B), 0); // Subtle active teal
        } else {
            lv_obj_set_style_border_color(btn, lv_color_hex(0x374151), 0);
            lv_obj_set_style_bg_color(btn, lv_color_hex(0x1F2937), 0);
        }
    };

    update_btn_style(_btn_balanced, POWER_PROFILE_BALANCED);
    update_btn_style(_btn_school, POWER_PROFILE_SCHOOL);
    update_btn_style(_btn_saver, POWER_PROFILE_SUPER_SAVER);

    if (_lbl_wrist_toggle) {
        if (wrist_on) {
            lv_label_set_text(_lbl_wrist_toggle, "Kol Kaldirma Uykusu: ACIK");
            lv_obj_set_style_text_color(_lbl_wrist_toggle, lv_color_hex(0x34D399), 0);
        } else {
            lv_label_set_text(_lbl_wrist_toggle, "Kol Kaldirma: KAPALI (Tasarruf)");
            lv_obj_set_style_text_color(_lbl_wrist_toggle, lv_color_hex(0xF59E0B), 0);
        }
    }
}
