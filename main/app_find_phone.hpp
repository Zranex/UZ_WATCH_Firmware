#pragma once

#include "esp_brookesia.hpp"
#include <string>

class AppFindPhone : public esp_brookesia::systems::phone::App {
public:
    AppFindPhone();
    ~AppFindPhone();

    void reset_state();
    void request_close() { notifyCoreClosed(); }
    static AppFindPhone* get_instance();

protected:
    bool run() override;
    bool back() override;
    bool close() override;

private:
    lv_obj_t* _bg_obj;
    lv_obj_t* _btn_back;
    lv_obj_t* _btn_alarm;
    lv_obj_t* _lbl_alarm;
    lv_obj_t* _status_label;
    lv_timer_t* _poll_timer;

    bool _is_alarming;

    static void on_back_clicked(lv_event_t *e);
    static void btn_alarm_cb(lv_event_t *e);
    void update_ui();
};
