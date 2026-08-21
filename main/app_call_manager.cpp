#include "app_call_manager.hpp"
#include "ble_manager.h"
#include "esp_log.h"
#include "bsp/esp-bsp.h"

#define TAG "CallManager"

lv_obj_t* CallManager::_modal = nullptr;

void CallManager::init() {
    _modal = nullptr;
}

void CallManager::show_incoming_call(const char* caller_name) {
    if (bsp_display_lock(100)) {
        if (_modal != nullptr) {
            lv_obj_del(_modal);
            _modal = nullptr;
        }

        _modal = lv_obj_create(lv_layer_top());
        lv_obj_set_size(_modal, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_bg_color(_modal, lv_color_hex(0x111111), 0);
        lv_obj_set_style_border_width(_modal, 0, 0);
        lv_obj_clear_flag(_modal, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* icon = lv_label_create(_modal);
        lv_label_set_text(icon, LV_SYMBOL_CALL);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_20, 0);
        lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 40);
        
        lv_obj_t* lbl_title = lv_label_create(_modal);
        lv_label_set_text(lbl_title, "Arama Geliyor");
        lv_obj_set_style_text_color(lbl_title, lv_color_hex(0xAAAAAA), 0);
        lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_16, 0);
        lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, 80);

        lv_obj_t* lbl_name = lv_label_create(_modal);
        lv_label_set_text(lbl_name, caller_name);
        lv_obj_set_style_text_color(lbl_name, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(lbl_name, &lv_font_montserrat_20, 0);
        lv_obj_align(lbl_name, LV_ALIGN_TOP_MID, 0, 110);
        
        lv_obj_t* btn_reject = lv_btn_create(_modal);
        lv_obj_set_size(btn_reject, 130, 60);
        lv_obj_align(btn_reject, LV_ALIGN_BOTTOM_LEFT, 20, -30);
        lv_obj_set_style_bg_color(btn_reject, lv_palette_main(LV_PALETTE_RED), 0);
        lv_obj_set_style_radius(btn_reject, 30, 0);
        lv_obj_add_event_cb(btn_reject, on_btn_reject_clicked, LV_EVENT_CLICKED, nullptr);
        
        lv_obj_t* lbl_reject = lv_label_create(btn_reject);
        lv_label_set_text(lbl_reject, LV_SYMBOL_CLOSE " Reddet");
        lv_obj_set_style_text_font(lbl_reject, &lv_font_montserrat_16, 0);
        lv_obj_center(lbl_reject);

        lv_obj_t* btn_mute = lv_btn_create(_modal);
        lv_obj_set_size(btn_mute, 130, 60);
        lv_obj_align(btn_mute, LV_ALIGN_BOTTOM_RIGHT, -20, -30);
        lv_obj_set_style_bg_color(btn_mute, lv_palette_main(LV_PALETTE_GREY), 0);
        lv_obj_set_style_radius(btn_mute, 30, 0);
        lv_obj_add_event_cb(btn_mute, on_btn_mute_clicked, LV_EVENT_CLICKED, nullptr);

        lv_obj_t* lbl_mute = lv_label_create(btn_mute);
        lv_label_set_text(lbl_mute, LV_SYMBOL_MUTE " Sessiz");
        lv_obj_set_style_text_font(lbl_mute, &lv_font_montserrat_16, 0);
        lv_obj_center(lbl_mute);

        bsp_display_unlock();
    }
}

void CallManager::hide_incoming_call() {
    if (bsp_display_lock(100)) {
        if (_modal != nullptr) {
            lv_obj_del(_modal);
            _modal = nullptr;
        }
        bsp_display_unlock();
    }
}

void CallManager::on_btn_reject_clicked(lv_event_t* e) {
    ble_manager_send_media_command("CALL_REJECT");
    hide_incoming_call();
}

void CallManager::on_btn_mute_clicked(lv_event_t* e) {
    ble_manager_send_media_command("CALL_MUTE");
    lv_obj_t* btn = (lv_obj_t*)lv_event_get_target(e);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x555555), 0);
}

extern "C" void app_call_manager_show_incoming_call(const char* caller_name) {
    CallManager::show_incoming_call(caller_name);
}

extern "C" void app_call_manager_hide_incoming_call() {
    CallManager::hide_incoming_call();
}
