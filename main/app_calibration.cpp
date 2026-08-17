#include "app_calibration.hpp"
#include "esp_lib_utils.h"

// Initialize static variables
float AppCalibration::_baseline_x = 0.0f;
float AppCalibration::_baseline_y = 0.0f;
float AppCalibration::_baseline_z = 1.0f; // Default assuming watch face up
bool AppCalibration::_is_loaded = false;

AppCalibration::AppCalibration() 
    : esp_brookesia::systems::phone::App("Kalibrasyon", nullptr, true) 
{
    load_calibration();
}

AppCalibration::~AppCalibration() {
}

bool AppCalibration::run() {
    lv_obj_t * screen = lv_scr_act();
    if (!screen) return false;

    lv_obj_set_style_bg_color(screen, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    _label_info = lv_label_create(screen);
    lv_label_set_text(_label_info, "Saati ekranina\nbaktigin acidaki\ngibi tut ve\nKalibre Et tusuna bas.");
    lv_obj_set_style_text_color(_label_info, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_align(_label_info, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(_label_info, LV_ALIGN_TOP_MID, 0, 80);

    _btn_calibrate = lv_btn_create(screen);
    lv_obj_set_size(_btn_calibrate, 200, 60);
    lv_obj_align(_btn_calibrate, LV_ALIGN_CENTER, 0, 40);
    lv_obj_set_style_bg_color(_btn_calibrate, lv_color_hex(0x007BFF), 0);
    lv_obj_add_event_cb(_btn_calibrate, on_calibrate_btn_clicked, LV_EVENT_CLICKED, this);

    lv_obj_t * btn_label = lv_label_create(_btn_calibrate);
    lv_label_set_text(btn_label, "Kalibre Et");
    lv_obj_center(btn_label);

    _label_status = lv_label_create(screen);
    lv_label_set_text(_label_status, "Mevcut: Bekleniyor");
    lv_obj_set_style_text_color(_label_status, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(_label_status, LV_ALIGN_BOTTOM_MID, 0, -40);

    return true;
}

bool AppCalibration::back() {
    return true;
}

void AppCalibration::on_calibrate_btn_clicked(lv_event_t* e) {
    AppCalibration* app = (AppCalibration*)lv_event_get_user_data(e);
    if (!app) return;

    qmi8658_acc_t acc;
    if (qmi8658_read_acc(&acc) == ESP_OK) {
        save_calibration(acc.x, acc.y, acc.z);
        
        char buf[64];
        snprintf(buf, sizeof(buf), "Kaydedildi!\nX:%.2f Y:%.2f Z:%.2f", acc.x, acc.y, acc.z);
        lv_label_set_text(app->_label_status, buf);
    } else {
        lv_label_set_text(app->_label_status, "HATA: Sensor Okunamadi");
    }
}

void AppCalibration::save_calibration(float x, float y, float z) {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);
    if (err == ESP_OK) {
        // NVS doesn't support float directly, save as integer * 1000
        nvs_set_i32(my_handle, "calib_x", (int32_t)(x * 1000.0f));
        nvs_set_i32(my_handle, "calib_y", (int32_t)(y * 1000.0f));
        nvs_set_i32(my_handle, "calib_z", (int32_t)(z * 1000.0f));
        nvs_commit(my_handle);
        nvs_close(my_handle);
        
        _baseline_x = x;
        _baseline_y = y;
        _baseline_z = z;
    }
}

void AppCalibration::load_calibration() {
    if (_is_loaded) return;
    
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("storage", NVS_READONLY, &my_handle);
    if (err == ESP_OK) {
        int32_t x_i=0, y_i=0, z_i=1000;
        nvs_get_i32(my_handle, "calib_x", &x_i);
        nvs_get_i32(my_handle, "calib_y", &y_i);
        nvs_get_i32(my_handle, "calib_z", &z_i);
        nvs_close(my_handle);
        
        _baseline_x = (float)x_i / 1000.0f;
        _baseline_y = (float)y_i / 1000.0f;
        _baseline_z = (float)z_i / 1000.0f;
    }
    _is_loaded = true;
}

void AppCalibration::get_calibrated_baseline(float *x, float *y, float *z) {
    if (!_is_loaded) load_calibration();
    *x = _baseline_x;
    *y = _baseline_y;
    *z = _baseline_z;
}
