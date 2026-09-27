#include "app_find_phone.hpp"
extern const lv_image_dsc_t icon_find_phone;
#include "esp_log.h"
#include "bsp/esp-bsp.h"

extern "C" {
#include "ble_manager.h"
}

#define TAG "FindPhone"

static AppFindPhone* s_find_phone_instance = nullptr;

extern "C" void app_find_phone_handle_stop_ack(void) {
    if (s_find_phone_instance) {
        if (bsp_display_lock(1000)) {
            s_find_phone_instance->reset_state();
            bsp_display_unlock();
        }
    }
}

AppFindPhone* AppFindPhone::get_instance() {
    return s_find_phone_instance;
}

AppFindPhone::AppFindPhone() 
    : esp_brookesia::systems::phone::App("Tel Bul", &icon_find_phone, true),
      _bg_obj(nullptr), _btn_back(nullptr), _btn_alarm(nullptr), _lbl_alarm(nullptr), 
      _status_label(nullptr), _poll_timer(nullptr), _is_alarming(false) {
    s_find_phone_instance = this;
}

AppFindPhone::~AppFindPhone() {
    if (s_find_phone_instance == this) {
        s_find_phone_instance = nullptr;
    }
}

void AppFindPhone::on_back_clicked(lv_event_t *e) {
    AppFindPhone *self = (AppFindPhone *)lv_event_get_user_data(e);
    if (self) {
        self->request_close();
    }
}

bool AppFindPhone::run() {
    if (_bg_obj != nullptr) return true;
    s_find_phone_instance = this;

    lv_obj_t* scr = lv_scr_act();
    _bg_obj = lv_obj_create(scr);
    lv_obj_set_size(_bg_obj, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(_bg_obj, lv_color_hex(0x121212), 0);
    lv_obj_set_style_border_width(_bg_obj, 0, 0);
    lv_obj_set_style_radius(_bg_obj, 0, 0);
    lv_obj_clear_flag(_bg_obj, LV_OBJ_FLAG_SCROLLABLE);

    // 1. Back Button (<) at Top-Left
    _btn_back = lv_btn_create(_bg_obj);
    lv_obj_set_size(_btn_back, 44, 44);
    lv_obj_align(_btn_back, LV_ALIGN_TOP_LEFT, 15, 15);
    lv_obj_set_style_radius(_btn_back, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(_btn_back, lv_color_hex(0x222222), 0);
    lv_obj_set_style_border_color(_btn_back, lv_color_hex(0x444444), 0);
    lv_obj_set_style_border_width(_btn_back, 1, 0);
    lv_obj_add_event_cb(_btn_back, on_back_clicked, LV_EVENT_CLICKED, this);

    lv_obj_t* lbl_back = lv_label_create(_btn_back);
    lv_obj_set_style_text_font(lbl_back, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(lbl_back, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(lbl_back, LV_SYMBOL_LEFT);
    lv_obj_center(lbl_back);

    // 2. Title
    lv_obj_t* title_label = lv_label_create(_bg_obj);
    lv_label_set_text(title_label, "Telefonumu Bul");
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title_label, LV_ALIGN_TOP_LEFT, 70, 24);

    // 3. Status Label
    _status_label = lv_label_create(_bg_obj);
    lv_obj_set_style_text_font(_status_label, &lv_font_montserrat_14, 0);
    lv_obj_align(_status_label, LV_ALIGN_CENTER, 0, -110);

    // 4. Big Circular Alarm Button
    _btn_alarm = lv_btn_create(_bg_obj);
    lv_obj_set_size(_btn_alarm, 170, 170);
    lv_obj_align(_btn_alarm, LV_ALIGN_CENTER, 0, 20);
    lv_obj_set_style_radius(_btn_alarm, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_event_cb(_btn_alarm, btn_alarm_cb, LV_EVENT_CLICKED, this);
    
    _lbl_alarm = lv_label_create(_btn_alarm);
    lv_obj_set_style_text_font(_lbl_alarm, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_align(_lbl_alarm, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(_lbl_alarm);

    // 5. Periodic status refresh timer (checks BLE connection)
    _poll_timer = lv_timer_create([](lv_timer_t* t) {
        AppFindPhone* app = (AppFindPhone*)t->user_data;
        if (app) {
            app->update_ui();
        }
    }, 1000, this);

    update_ui();
    return true;
}

bool AppFindPhone::back() {
    return notifyCoreClosed();
}

bool AppFindPhone::close() {
    if (_poll_timer) {
        lv_timer_del(_poll_timer);
        _poll_timer = nullptr;
    }

    if (_is_alarming) {
        _is_alarming = false;
        ble_manager_send_media_command("FIND_PHONE_STOP");
    }

    if (_bg_obj != nullptr) {
        lv_obj_del(_bg_obj);
        _bg_obj = nullptr;
    }
    _btn_back = nullptr;
    _btn_alarm = nullptr;
    _lbl_alarm = nullptr;
    _status_label = nullptr;
    return true;
}

void AppFindPhone::reset_state() {
    _is_alarming = false;
    update_ui();
}

void AppFindPhone::update_ui() {
    if (!_status_label || !_btn_alarm || !_lbl_alarm) return;

    bool connected = ble_manager_is_connected();

    if (!connected) {
        lv_label_set_text(_status_label, "Bluetooth Bagli Degil!");
        lv_obj_set_style_text_color(_status_label, lv_color_hex(0xFFA500), 0);
        
        lv_obj_set_style_bg_color(_btn_alarm, lv_color_hex(0x333333), 0);
        lv_label_set_text(_lbl_alarm, LV_SYMBOL_WARNING "\nBAGLANTI\nYOK");
        _is_alarming = false;
        return;
    }

    if (_is_alarming) {
        lv_label_set_text(_status_label, "Telefon Caliniyor...");
        lv_obj_set_style_text_color(_status_label, lv_color_hex(0xFF5252), 0);
        
        lv_obj_set_style_bg_color(_btn_alarm, lv_color_hex(0xD32F2F), 0);
        lv_label_set_text(_lbl_alarm, LV_SYMBOL_MUTE "\nDURDUR");
    } else {
        lv_label_set_text(_status_label, "Bagli - Calmaya Hazir");
        lv_obj_set_style_text_color(_status_label, lv_color_hex(0x00E676), 0);
        
        lv_obj_set_style_bg_color(_btn_alarm, lv_color_hex(0x1976D2), 0);
        lv_label_set_text(_lbl_alarm, LV_SYMBOL_BELL "\nALARM\nCAL");
    }
}

void AppFindPhone::btn_alarm_cb(lv_event_t *e) {
    AppFindPhone *self = (AppFindPhone *)lv_event_get_user_data(e);
    if (!self) return;

    if (!ble_manager_is_connected()) {
        ESP_LOGW(TAG, "Cannot ring phone: BLE not connected");
        if (self->_status_label) {
            lv_label_set_text(self->_status_label, "Once Telefona Baglanin!");
            lv_obj_set_style_text_color(self->_status_label, lv_color_hex(0xFF5252), 0);
        }
        return;
    }

    if (self->_is_alarming) {
        self->_is_alarming = false;
        ESP_LOGI(TAG, "Stopping Find Phone alarm...");
        ble_manager_send_media_command("FIND_PHONE_STOP");
    } else {
        self->_is_alarming = true;
        ESP_LOGI(TAG, "Starting Find Phone alarm...");
        ble_manager_send_media_command("FIND_PHONE_START");
    }
    self->update_ui();
}
