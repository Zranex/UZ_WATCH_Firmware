#pragma once

#include "esp_brookesia.hpp"
#include "lvgl.h"

extern "C" {
void app_tars_on_triple_tap_wake(void);
void app_tars_set_api_key_from_ble(const char* key);
void app_tars_handle_reply_from_ble(const char* payload);
void app_tars_stop_all_audio(void);
}

enum TarsVisState {
    TARS_VIS_IDLE = 0,
    TARS_VIS_LISTEN = 1,
    TARS_VIS_THINK = 2,
    TARS_VIS_REPLY = 3,
};

class AppTars : public esp_brookesia::systems::phone::App {
public:
    AppTars();
    ~AppTars() override;

    bool run() override;
    bool back() override;
    bool close() override;

    static AppTars* get_instance();
    void update_ui_state(const char* status_text, uint32_t status_color, int mic_level_pct,
                         const char* user_text, const char* tars_text, const char* action_text,
                         const char* trigger_btn_text = nullptr);
    void set_visual_state(TarsVisState state, int mic_level_pct = -1);
    void set_turbo_mode(bool enabled);
    bool is_turbo_mode() const { return _turbo_mode; }
    void cycle_tone();
    void cycle_style();
    void render_monolith_frame();

    bool is_open() const { return _bg_obj != nullptr; }
    void request_close() { notifyCoreClosed(); }
    int get_humor() const { return _humor_pct; }
    void set_humor(int h);

private:
    void apply_theme_chrome();
    void drain_ui_mailbox();
    void save_ui_prefs();
    void load_ui_prefs();

    lv_obj_t* _bg_obj;
    lv_obj_t* _btn_back;
    lv_obj_t* _title_label;
    lv_obj_t* _params_label;
    lv_obj_t* _btn_tone;
    lv_obj_t* _btn_tone_label;
    lv_obj_t* _monolith_img;
    lv_obj_t* _status_label;
    lv_obj_t* _dialog_card;
    lv_obj_t* _user_label;
    lv_obj_t* _tars_label;
    lv_obj_t* _action_badge;
    lv_obj_t* _btn_turbo;
    lv_obj_t* _btn_turbo_label;
    lv_obj_t* _btn_humor;
    lv_obj_t* _btn_humor_label;
    lv_obj_t* _btn_test;
    lv_obj_t* _btn_test_label;
    lv_timer_t* _anim_timer;

    int _humor_pct;
    int _honesty_pct;
    int _monolith_tone;   // 0=Beyaz, 1=Celik, 2=Amber, 3=Yesil
    int _monolith_style;  // 0=Saf TARS, 1=NASA Telemetri, 2=Asimetrik
    volatile bool _turbo_mode;
    volatile int _vis_state;
    volatile int _live_mic_pct;
    volatile int64_t _reply_anim_until_us;
    int _cached_bat_pct;
    bool _cached_charging;
    int64_t _last_bat_check_us;
    uint32_t _anim_frame;
    uint32_t _idle_skip_div;
};
