#pragma once

#include "lvgl.h"
#include "esp_brookesia.hpp"

class AppLockscreen {
public:
    static void show(esp_brookesia::systems::phone::Phone* phone);
    static void show_again();
private:
    static lv_obj_t* lock_scr;
    static esp_brookesia::systems::phone::Phone* _phone;
    
    static lv_obj_t* label_hour;
    static lv_obj_t* label_minute;
    static lv_obj_t* label_second;
    static lv_obj_t* label_date;
    static lv_timer_t* timer_clock;

    static void event_cb(lv_event_t * e);
    static void anim_deleted_cb(lv_anim_t * a);
    static void timer_cb(lv_timer_t * t);
};
