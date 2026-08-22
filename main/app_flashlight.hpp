#pragma once

#include "esp_brookesia.hpp"

class AppFlashlight : public esp_brookesia::systems::phone::App {
public:
    AppFlashlight();
    ~AppFlashlight() override;

protected:
    bool run() override;
    bool back() override;
    bool close() override;

private:
    lv_obj_t* _bg_obj;
    
    lv_obj_t* _btn_sos;
    lv_obj_t* _btn_toggle;
    lv_obj_t* _lbl_toggle;
    
    bool _is_on;
    bool _is_sos;
    
    lv_timer_t* _sos_timer;
    int _sos_step;
    int _sos_tick;

    static void on_btn_toggle_clicked(lv_event_t* e);
    static void on_btn_sos_clicked(lv_event_t* e);
    static void sos_timer_cb(lv_timer_t* t);
    
    void set_light(bool on);
};
