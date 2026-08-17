#pragma once

#include "esp_brookesia.hpp"
#include <nvs_flash.h>
#include <nvs.h>

#ifdef __cplusplus
extern "C" {
#endif
#include "qmi8658.h"
#ifdef __cplusplus
}
#endif

class AppCalibration : public esp_brookesia::systems::phone::App {
public:
    AppCalibration();
    ~AppCalibration();

    static void get_calibrated_baseline(float *x, float *y, float *z);

protected:
    bool run() override;
    bool back() override;

private:
    static void on_calibrate_btn_clicked(lv_event_t* e);
    static void save_calibration(float x, float y, float z);
    static void load_calibration();

    lv_obj_t* _label_info;
    lv_obj_t* _btn_calibrate;
    lv_obj_t* _label_status;
    
    static float _baseline_x;
    static float _baseline_y;
    static float _baseline_z;
    static bool _is_loaded;
};
