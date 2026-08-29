#pragma once
#include "esp_brookesia.hpp"

class AppWiFiTest : public esp_brookesia::systems::phone::App {
public:
    AppWiFiTest();
    ~AppWiFiTest();
    bool run() override;
    bool back() override;
    bool close() override;

    lv_obj_t* _bg_obj;
    lv_obj_t* _label_status;
    lv_obj_t* _btn;
    lv_timer_t* _timer;
};
