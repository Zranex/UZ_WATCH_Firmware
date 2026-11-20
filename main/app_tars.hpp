#pragma once

#include "esp_brookesia.hpp"
#include "lvgl.h"

extern "C" {
void app_tars_on_triple_tap_wake(void);
void app_tars_set_api_key_from_ble(const char* key);
void app_tars_handle_reply_from_ble(const char* payload);
void app_tars_stop_all_audio(void);
}

class AppTars : public esp_brookesia::systems::phone::App {
public:
    AppTars();
    ~AppTars() override;

    bool run() override;
    bool back() override;
    bool close() override;

    static AppTars* get_instance();
    void update_ui_state(const char* status_text, uint32_t status_color, int mic_level_pct,
                         const char* user_text, const char* tars_text, const char* action_text);

    bool is_open() const { return _bg_obj != nullptr; }
    int get_humor() const { return _humor_pct; }
    void set_humor(int h);

private:
    lv_obj_t* _bg_obj;
    lv_obj_t* _params_label;
    lv_obj_t* _status_label;
    lv_obj_t* _level_bar;
    lv_obj_t* _user_label;
    lv_obj_t* _tars_label;
    lv_obj_t* _action_badge;
    lv_obj_t* _btn_humor;
    lv_obj_t* _btn_test;

    int _humor_pct;
    int _honesty_pct;
};
