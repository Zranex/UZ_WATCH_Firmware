#pragma once

#include "lvgl.h"
#include <string>

class AppNotifications {
public:
    static void init();
    static void show(const char* sender, const char* message, const std::string& notif_id = "");
    static void hide();

private:
    static lv_obj_t* container;
    static lv_obj_t* title_label;
    static lv_obj_t* message_label;
    static lv_obj_t* reply_btn;
    static lv_timer_t* hide_timer;
    static std::string current_banner_notif_id;

    static void hide_timer_cb(lv_timer_t* timer);
    static void container_event_cb(lv_event_t* e);
    static void on_banner_reply_clicked(lv_event_t* e);
    static void on_banner_reply_selected(lv_event_t* e);
    static void on_banner_user_data_deleted(lv_event_t* e);
};

// C wrapper for BLE manager
#ifdef __cplusplus
extern "C" {
#endif

void app_notifications_show_from_ble(const char* payload);
void app_notifications_show_system_alert(const char* msg);

#ifdef __cplusplus
}
#endif
