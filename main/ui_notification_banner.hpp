#pragma once

#include "lvgl.h"
#include <string>

class AppNotifications {
public:
    static constexpr int QUICK_REPLY_COUNT = 4;

    static void init();
    static void show(const char* sender, const char* message, const std::string& notif_id = "");
    static void hide();

    // Custom Quick Replies (persisted in NVS & synced over BLE)
    static void load_quick_replies_from_nvs();
    static void update_quick_replies(const char* payload);
    static std::string get_quick_reply(int index);

    // Live Google Maps Turn-by-Turn Navigation Overlay (returns true if maneuver/street changed)
    static bool show_nav(char dir_code, const char* dist_text, const char* street_text);
    static void hide_nav();

private:
    static lv_obj_t* container;
    static lv_obj_t* title_label;
    static lv_obj_t* message_label;
    static lv_obj_t* reply_btn;
    static lv_timer_t* hide_timer;
    static std::string current_banner_notif_id;

    // Navigation overlay widgets
    static lv_obj_t* nav_container;
    static lv_obj_t* nav_icon_box;
    static lv_obj_t* nav_icon_label;
    static lv_obj_t* nav_dist_label;
    static lv_obj_t* nav_street_label;
    static lv_timer_t* nav_timeout_timer;
    static char last_nav_dir;
    static std::string last_nav_street;
    static bool nav_user_dismissed_current_step;

    static void hide_timer_cb(lv_timer_t* timer);
    static void container_event_cb(lv_event_t* e);
    static void on_banner_reply_clicked(lv_event_t* e);
    static void on_banner_reply_selected(lv_event_t* e);
    static void on_banner_user_data_deleted(lv_event_t* e);

    static void nav_container_event_cb(lv_event_t* e);
    static void nav_timeout_timer_cb(lv_timer_t* timer);
};

// C wrapper for BLE manager
#ifdef __cplusplus
extern "C" {
#endif

void app_notifications_show_from_ble(const char* payload);
void app_notifications_show_system_alert(const char* msg);
void app_notifications_update_quick_replies(const char* payload);
void app_notifications_update_nav_from_ble(const char* payload);
void app_notifications_hide_nav_from_ble(void);

#ifdef __cplusplus
}
#endif
