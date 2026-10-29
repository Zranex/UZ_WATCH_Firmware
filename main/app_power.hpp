#pragma once

#include "esp_brookesia.hpp"
#include "lvgl.h"
#include <string>

enum PowerProfile {
    POWER_PROFILE_BALANCED = 0,
    POWER_PROFILE_SCHOOL,
    POWER_PROFILE_SUPER_SAVER
};

class AppPower : public esp_brookesia::systems::phone::App {
public:
    AppPower();
    virtual ~AppPower();

    virtual bool run() override;
    virtual bool back() override;
    virtual bool close() override;
    virtual bool init() override;

    void request_close() { notifyCoreClosed(); }
    void update_ui();

    static AppPower* get_instance() { return _instance; }
    static PowerProfile get_active_profile() { return _active_profile; }
    static void set_power_profile(PowerProfile profile);

private:
    static AppPower* _instance;
    static PowerProfile _active_profile;

    lv_obj_t* _bg_obj;
    lv_obj_t* _btn_back;
    lv_obj_t* _arc_battery;
    lv_obj_t* _lbl_percent;
    lv_obj_t* _lbl_voltage;
    lv_obj_t* _lbl_charging;
    lv_obj_t* _lbl_estimate;

    lv_obj_t* _btn_balanced;
    lv_obj_t* _btn_school;
    lv_obj_t* _btn_saver;
    lv_obj_t* _btn_wrist_toggle;
    lv_obj_t* _lbl_wrist_toggle;
    lv_obj_t* _btn_sleep_now;

    lv_timer_t* _poll_timer;

    static void on_back_clicked(lv_event_t* e);
    static void on_profile_clicked(lv_event_t* e);
    static void on_wrist_toggle_clicked(lv_event_t* e);
    static void on_sleep_now_clicked(lv_event_t* e);
};
