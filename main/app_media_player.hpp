#pragma once

#include "esp_brookesia.hpp"
#include <string>

class AppMediaPlayer : public esp_brookesia::systems::phone::App {
public:
    AppMediaPlayer();
    ~AppMediaPlayer();

    // C-compatible wrapper will call this
    static void update_media_data(const char* source, const char* title, const char* artist, const char* state);

protected:
    bool run() override;
    bool back() override;
    bool close() override;

private:
    static void on_btn_prev_clicked(lv_event_t* e);
    static void on_btn_play_clicked(lv_event_t* e);
    static void on_btn_next_clicked(lv_event_t* e);
    
    void apply_theme(const char* source);
    void send_media_command(const char* cmd);

    lv_obj_t* _bg_obj;
    lv_obj_t* _label_source;
    lv_obj_t* _label_title;
    lv_obj_t* _label_artist;
    
    lv_obj_t* _btn_prev;
    lv_obj_t* _btn_play;
    lv_obj_t* _btn_next;
    lv_obj_t* _label_play_icon;

    static AppMediaPlayer* _instance;
    
    // State storage
    std::string _current_source;
    std::string _current_state;
};

// C wrapper for BLE callbacks
#ifdef __cplusplus
extern "C" {
#endif

void app_media_player_update_from_ble(const char* payload);

#ifdef __cplusplus
}
#endif
