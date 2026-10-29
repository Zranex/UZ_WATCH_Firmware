#pragma once

#include "esp_brookesia.hpp"
#include "lvgl.h"
#include <string>

class AppSchool : public esp_brookesia::systems::phone::App {
public:
    AppSchool();
    virtual ~AppSchool();

    virtual bool run() override;
    virtual bool back() override;
    virtual bool close() override;
    virtual bool init() override;

    void request_close() { notifyCoreClosed(); }
    void update_ui();

    static void check_bell_tick();
    static bool is_lesson_in_progress();
    static void start_background_timer();
    static AppSchool* get_instance() { return _instance; }

private:
    static AppSchool* _instance;
    lv_obj_t* _bg_obj;
    lv_obj_t* _btn_back;
    lv_obj_t* _arc_progress;
    lv_obj_t* _lbl_status;
    lv_obj_t* _lbl_time_left;
    lv_obj_t* _lbl_next;
    lv_obj_t* _list_schedule;
    lv_timer_t* _poll_timer;

    static void on_back_clicked(lv_event_t* e);
};
