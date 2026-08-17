#pragma once

#include "app.hpp"
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
    
    void send_command(const char* cmd);

    lv_obj_t* _bg_obj;
};
