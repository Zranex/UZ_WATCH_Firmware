#pragma once

#include "esp_brookesia.hpp"
#include <string>

class AppFindPhone : public esp_brookesia::systems::phone::App {
public:
    AppFindPhone();
    ~AppFindPhone();

protected:
    bool run() override;
    bool back() override;
    bool close() override;

private:
    lv_obj_t* _btn_alarm;
    lv_obj_t* _lbl_alarm;
    lv_obj_t* _status_label;

    bool _is_alarming;

    static void btn_alarm_cb(lv_event_t *e);
    void update_ui();
};
