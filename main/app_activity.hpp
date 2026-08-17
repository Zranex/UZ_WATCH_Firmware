#pragma once

#include "esp_brookesia.hpp"

class AppActivity : public esp_brookesia::systems::phone::App {
public:
    AppActivity();
    ~AppActivity();

protected:
    bool run() override;
    bool back() override;
    bool close() override;

private:
    lv_obj_t* _bg_obj = nullptr;
    lv_obj_t* _arc_steps = nullptr;
    lv_obj_t* _label_steps = nullptr;
    lv_obj_t* _label_desc = nullptr;
    lv_timer_t* _timer = nullptr;

    static void timer_cb(lv_timer_t* t);
};
