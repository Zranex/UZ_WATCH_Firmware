#pragma once

#include "esp_brookesia.hpp"

class AppAirDrop : public esp_brookesia::systems::phone::App {
public:
    AppAirDrop();
    virtual ~AppAirDrop();

    bool run() override;
    bool back() override;
    bool close() override;
    void request_close() { notifyCoreClosed(); }

public:
    lv_obj_t* _bg_obj;
    lv_obj_t* _btn_back;
    lv_obj_t* _label_status;
    lv_obj_t* _label_desc;
    lv_obj_t* _btn_toggle_mode;
    lv_obj_t* _label_btn_mode;
    lv_timer_t* _poll_timer;
};


