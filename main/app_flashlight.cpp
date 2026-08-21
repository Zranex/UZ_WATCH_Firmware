#include "app_flashlight.hpp"
#include "esp_log.h"
#include "bsp/esp-bsp.h"

#define TAG "Flashlight"

AppFlashlight::AppFlashlight() 
    : esp_brookesia::systems::phone::App("El Feneri", LV_SYMBOL_POWER, true),
      _is_on(true), _is_sos(false), _sos_timer(nullptr), _sos_step(0), _sos_tick(0) {
}

AppFlashlight::~AppFlashlight() {}

bool AppFlashlight::run() {
    lv_obj_t* scr = lv_scr_act();
    lv_obj_clean(scr);

    _bg_obj = lv_obj_create(scr);
    lv_obj_set_size(_bg_obj, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(_bg_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(_bg_obj, 0, 0);
    lv_obj_set_style_radius(_bg_obj, 0, 0);

    // Toggle button in the middle
    _btn_toggle = lv_btn_create(_bg_obj);
    lv_obj_set_size(_btn_toggle, 100, 100);
    lv_obj_align(_btn_toggle, LV_ALIGN_CENTER, 0, -20);
    lv_obj_set_style_radius(_btn_toggle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(_btn_toggle, lv_color_hex(0x333333), 0);
    lv_obj_add_event_cb(_btn_toggle, on_btn_toggle_clicked, LV_EVENT_CLICKED, this);

    _lbl_toggle = lv_label_create(_btn_toggle);
    lv_label_set_text(_lbl_toggle, LV_SYMBOL_POWER);
    lv_obj_set_style_text_font(_lbl_toggle, &lv_font_montserrat_20, 0);
    lv_obj_center(_lbl_toggle);

    // SOS Button
    _btn_sos = lv_btn_create(_bg_obj);
    lv_obj_set_size(_btn_sos, 120, 50);
    lv_obj_align(_btn_sos, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_obj_set_style_bg_color(_btn_sos, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_add_event_cb(_btn_sos, on_btn_sos_clicked, LV_EVENT_CLICKED, this);

    lv_obj_t* lbl_sos = lv_label_create(_btn_sos);
    lv_label_set_text(lbl_sos, "SOS Mors");
    lv_obj_center(lbl_sos);

    _sos_timer = lv_timer_create(sos_timer_cb, 100, this);
    lv_timer_pause(_sos_timer);

    set_light(true);

    return true;
}

bool AppFlashlight::back() {
    return true; // Let Brookesia handle the back navigation
}

bool AppFlashlight::close() {
    if (_sos_timer) {
        lv_timer_del(_sos_timer);
        _sos_timer = nullptr;
    }
    _is_sos = false;
    
    // Restore normal brightness
    bsp_display_brightness_set(50);
    return true;
}

void AppFlashlight::set_light(bool on) {
    _is_on = on;
    if (_is_on) {
        lv_obj_set_style_bg_color(_bg_obj, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_color(_lbl_toggle, lv_color_hex(0xFFFFFF), 0);
        bsp_display_brightness_set(100);
    } else {
        lv_obj_set_style_bg_color(_bg_obj, lv_color_hex(0x000000), 0);
        lv_obj_set_style_text_color(_lbl_toggle, lv_color_hex(0x555555), 0);
        bsp_display_brightness_set(10); // Dim it so user can still see the button to turn it back on
    }
}

void AppFlashlight::on_btn_toggle_clicked(lv_event_t* e) {
    AppFlashlight* self = (AppFlashlight*)lv_event_get_user_data(e);
    if (self->_is_sos) {
        // Cancel SOS mode
        self->_is_sos = false;
        lv_timer_pause(self->_sos_timer);
    }
    self->set_light(!self->_is_on);
}

void AppFlashlight::on_btn_sos_clicked(lv_event_t* e) {
    AppFlashlight* self = (AppFlashlight*)lv_event_get_user_data(e);
    self->_is_sos = !self->_is_sos;
    
    if (self->_is_sos) {
        self->_sos_step = 0;
        self->_sos_tick = 0;
        lv_timer_resume(self->_sos_timer);
    } else {
        lv_timer_pause(self->_sos_timer);
        self->set_light(true);
    }
}

void AppFlashlight::sos_timer_cb(lv_timer_t* t) {
    AppFlashlight* self = (AppFlashlight*)t->user_data;
    if (!self->_is_sos) return;

    // S O S pattern in morse:
    // S: 3 short flashes (on 1, off 1)
    // O: 3 long flashes (on 3, off 1)
    // S: 3 short flashes (on 1, off 1)
    // Delay between letters: 3 ticks
    // Delay between words: 7 ticks
    
    // Let's simplify the array: 1 = ON for 100ms, 0 = OFF for 100ms
    static const int sos_pattern[] = {
        1,0, 1,0, 1,0,       // S
        0,0,                 // space
        1,1,1,0, 1,1,1,0, 1,1,1,0, // O
        0,0,                 // space
        1,0, 1,0, 1,0,       // S
        0,0,0,0,0,0,0        // word space
    };
    
    int max_steps = sizeof(sos_pattern) / sizeof(sos_pattern[0]);
    
    if (self->_sos_step < max_steps) {
        self->set_light(sos_pattern[self->_sos_step] == 1);
        self->_sos_step++;
    } else {
        self->_sos_step = 0;
    }
}
