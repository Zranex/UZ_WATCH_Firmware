#pragma once

#include "esp_brookesia.hpp"
#include "lvgl.h"

class AppSmartHome : public esp_brookesia::systems::phone::App {
public:
    AppSmartHome();
    ~AppSmartHome() override;

    bool run() override;
    bool back() override;
    bool close() override;

private:
    static void on_btn_clicked(lv_event_t* e);
    static void on_wifi_toggle_clicked(lv_event_t* e);
    static void on_timer_tick(lv_timer_t* t);
    
    void send_command(const char* cmd, const char* friendly_name);
    void update_wifi_ui();

    lv_obj_t* _bg_obj;
    lv_obj_t* _wifi_status_label;
    lv_obj_t* _wifi_ip_label;
    lv_obj_t* _wifi_btn;
    lv_obj_t* _wifi_btn_label;
    lv_obj_t* _feedback_label;
    lv_timer_t* _timer;
};

