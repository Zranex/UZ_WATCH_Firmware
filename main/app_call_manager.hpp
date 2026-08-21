#pragma once
#include "lvgl.h"

class CallManager {
public:
    static void init();
    static void show_incoming_call(const char* caller_name);
    static void hide_incoming_call();
    
private:
    static lv_obj_t* _modal;
    static void on_btn_reject_clicked(lv_event_t* e);
    static void on_btn_mute_clicked(lv_event_t* e);
};
