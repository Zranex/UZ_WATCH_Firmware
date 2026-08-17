#pragma once

#include "lvgl.h"

class AppNotifications {
public:
    static void init();
    static void show(const char* sender, const char* message);
    static void hide();

private:
    static lv_obj_t* container;
    static lv_obj_t* title_label;
    static lv_obj_t* message_label;
    static lv_timer_t* hide_timer;

    static void hide_timer_cb(lv_timer_t* timer);
    static void container_event_cb(lv_event_t* e);
};

// C wrapper for BLE manager
#ifdef __cplusplus
extern "C" {
#endif

void app_notifications_show_from_ble(const char* payload);

#ifdef __cplusplus
}
#endif
