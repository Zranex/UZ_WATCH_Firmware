#pragma once

#include "esp_brookesia.hpp"

class AppAirDrop : public esp_brookesia::systems::phone::App {
public:
    AppAirDrop();
    virtual ~AppAirDrop();

    bool run() override;
    bool back() override;
    bool close() override;

private:
    lv_obj_t* _bg_obj;
    lv_obj_t* _label_status;
    lv_obj_t* _label_desc;
};


