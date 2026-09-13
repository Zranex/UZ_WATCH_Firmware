#include "app_find_phone.hpp"
extern const lv_image_dsc_t icon_find_phone;
#include "esp_log.h"
#include "ble_manager.h" // We use this to send BLE commands

#define TAG "FindPhone"

AppFindPhone::AppFindPhone() 
    : esp_brookesia::systems::phone::App("Tel Bul", &icon_find_phone, true),
      _bg_obj(nullptr), _btn_alarm(nullptr), _lbl_alarm(nullptr), _status_label(nullptr),
      _is_alarming(false) {
}

AppFindPhone::~AppFindPhone() {}

bool AppFindPhone::run() {
    if (_bg_obj != nullptr) return true;

    _bg_obj = lv_obj_create(lv_scr_act());
    lv_obj_set_size(_bg_obj, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(_bg_obj, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(_bg_obj, 0, 0);
    lv_obj_set_style_radius(_bg_obj, 0, 0);
    lv_obj_clear_flag(_bg_obj, LV_OBJ_FLAG_SCROLLABLE);

    // UI: Title
    lv_obj_t* title_label = lv_label_create(_bg_obj);
    lv_label_set_text(title_label, "Telefonumu Bul");
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 20);

    // UI: Status
    _status_label = lv_label_create(_bg_obj);
    lv_label_set_text(_status_label, "Hazir");
    lv_obj_align(_status_label, LV_ALIGN_TOP_MID, 0, 50);

    // UI: Big Alarm Button
    _btn_alarm = lv_btn_create(_bg_obj);
    lv_obj_set_size(_btn_alarm, 150, 150);
    lv_obj_align(_btn_alarm, LV_ALIGN_CENTER, 0, 10);
    lv_obj_set_style_radius(_btn_alarm, LV_RADIUS_CIRCLE, 0); // Make it a circle
    lv_obj_add_event_cb(_btn_alarm, btn_alarm_cb, LV_EVENT_CLICKED, this);
    
    _lbl_alarm = lv_label_create(_btn_alarm);
    lv_label_set_text(_lbl_alarm, LV_SYMBOL_BELL "\nALARM\nCAL");
    lv_obj_set_style_text_align(_lbl_alarm, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(_lbl_alarm);

    update_ui();
    return true;
}

bool AppFindPhone::back() {
    return close();
}

bool AppFindPhone::close() {
    if (_is_alarming) {
        _is_alarming = false;
        ble_manager_send_media_command("FIND_PHONE_STOP");
    }
    if (_bg_obj != nullptr) {
        lv_obj_del(_bg_obj);
        _bg_obj = nullptr;
    }
    _btn_alarm = nullptr;
    _lbl_alarm = nullptr;
    _status_label = nullptr;
    return true;
}

void AppFindPhone::update_ui() {
    if (!_status_label || !_btn_alarm || !_lbl_alarm) return;

    if (_is_alarming) {
        lv_label_set_text(_status_label, "Araniyor...");
        lv_obj_set_style_bg_color(_btn_alarm, lv_color_hex(0xFF0000), 0);
        lv_label_set_text(_lbl_alarm, LV_SYMBOL_MUTE "\nDURDUR");
    } else {
        lv_label_set_text(_status_label, "Hazir");
        lv_obj_set_style_bg_color(_btn_alarm, lv_palette_main(LV_PALETTE_BLUE), 0);
        lv_label_set_text(_lbl_alarm, LV_SYMBOL_BELL "\nALARM\nCAL");
    }
}

void AppFindPhone::btn_alarm_cb(lv_event_t *e) {
    AppFindPhone *self = (AppFindPhone *)lv_event_get_user_data(e);
    if (!self) return;

    if (self->_is_alarming) {
        self->_is_alarming = false;
        ble_manager_send_media_command("FIND_PHONE_STOP");
    } else {
        self->_is_alarming = true;
        ble_manager_send_media_command("FIND_PHONE_START");
    }
    self->update_ui();
}
