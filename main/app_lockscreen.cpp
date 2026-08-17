#include "app_lockscreen.hpp"
#include "esp_lib_utils.h"
extern "C" {
#include "rtc_lib.h"
}
#include <stdio.h>

extern const lv_image_dsc_t lockscreen_bg;
extern const lv_font_t font_cinzel_bold_160;
extern const lv_font_t font_cinzel_bold_54;
extern const lv_font_t font_cinzel_bold_36;

lv_obj_t* AppLockscreen::lock_scr = nullptr;
esp_brookesia::systems::phone::Phone* AppLockscreen::_phone = nullptr;

lv_obj_t* AppLockscreen::label_hour = nullptr;
lv_obj_t* AppLockscreen::label_minute = nullptr;
lv_obj_t* AppLockscreen::label_second = nullptr;
lv_obj_t* AppLockscreen::label_date = nullptr;
lv_timer_t* AppLockscreen::timer_clock = nullptr;

void AppLockscreen::anim_deleted_cb(lv_anim_t * a) {
    if (lock_scr) {
        lv_obj_delete(lock_scr);
        lock_scr = nullptr;
        ESP_UTILS_LOGI("Lockscreen object deleted.");
    }
}

void AppLockscreen::timer_cb(lv_timer_t * t) {
    if (!lock_scr) return;

    struct tm timeinfo;
    if (rtc_get_time(&timeinfo) == ESP_OK) {
        char buf[32];
        
        snprintf(buf, sizeof(buf), "%02d", timeinfo.tm_hour);
        lv_label_set_text(label_hour, buf);
        
        snprintf(buf, sizeof(buf), "%02d", timeinfo.tm_min);
        lv_label_set_text(label_minute, buf);
        
        snprintf(buf, sizeof(buf), "%02d", timeinfo.tm_sec);
        lv_label_set_text(label_second, buf);
        
        // Date format exactly as requested: DD/MM/YYYY
        snprintf(buf, sizeof(buf), "%02d/%02d/%d", timeinfo.tm_mday, timeinfo.tm_mon + 1, timeinfo.tm_year + 1900);
        lv_label_set_text(label_date, buf);
    }
}

void AppLockscreen::event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    
    if(code == LV_EVENT_GESTURE) {
        lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
        if(dir == LV_DIR_TOP) {
            ESP_UTILS_LOGI("Unlock gesture detected (Swipe Up). Loading main screen...");
            
            if (timer_clock) {
                lv_timer_delete(timer_clock);
                timer_clock = nullptr;
            }
            
            if (_phone) {
                lv_screen_load(_phone->getDisplay().getMainScreen());
                ESP_UTILS_LOGI("Switched to main screen.");
            }
            
            if (lock_scr) {
                lv_obj_delete(lock_scr);
                lock_scr = nullptr;
            }
        }
    }
}

void AppLockscreen::show_again() {
    if (_phone) {
        show(_phone);
    }
}

void AppLockscreen::show(esp_brookesia::systems::phone::Phone* phone) {
    if (lock_scr != nullptr) return;
    _phone = phone;

    lock_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(lock_scr, lv_color_hex(0x000000), 0); 
    lv_obj_set_style_bg_opa(lock_scr, LV_OPA_COVER, 0);
    lv_obj_remove_style(lock_scr, NULL, LV_PART_SCROLLBAR);
    
    lv_obj_t* img = lv_image_create(lock_scr);
    lv_image_set_src(img, &lockscreen_bg);
    lv_obj_center(img);
    
    label_hour = lv_label_create(lock_scr);
    lv_obj_set_style_text_font(label_hour, &font_cinzel_bold_160, 0);
    lv_obj_set_style_text_color(label_hour, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(label_hour, "00");
    // Center hour and shift it up further
    lv_obj_align(label_hour, LV_ALIGN_CENTER, 0, -120);
    
    label_minute = lv_label_create(lock_scr);
    lv_obj_set_style_text_font(label_minute, &font_cinzel_bold_160, 0);
    lv_obj_set_style_text_color(label_minute, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(label_minute, "00");
    // Center minute directly below hour
    lv_obj_align_to(label_minute, label_hour, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);
    
    label_second = lv_label_create(lock_scr);
    lv_obj_set_style_text_font(label_second, &font_cinzel_bold_54, 0);
    lv_obj_set_style_text_color(label_second, lv_color_hex(0xDDDDDD), 0);
    lv_label_set_text(label_second, "00");
    // Second to the right of minute, move further inward
    lv_obj_align_to(label_second, label_minute, LV_ALIGN_OUT_RIGHT_BOTTOM, -10, -15);
    
    label_date = lv_label_create(lock_scr);
    lv_obj_set_style_text_font(label_date, &font_cinzel_bold_36, 0);
    lv_obj_set_style_text_color(label_date, lv_color_hex(0xAAAAAA), 0);
    lv_label_set_text(label_date, "01/01/2026");
    // Date directly below minute
    lv_obj_align_to(label_date, label_minute, LV_ALIGN_OUT_BOTTOM_MID, 0, 15);
    
    timer_clock = lv_timer_create(timer_cb, 1000, nullptr);
    timer_cb(timer_clock);

    lv_obj_add_flag(lock_scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(lock_scr, event_cb, LV_EVENT_GESTURE, nullptr);
    
    lv_screen_load(lock_scr);
    ESP_UTILS_LOGI("Lockscreen loaded with custom clock design.");
}
