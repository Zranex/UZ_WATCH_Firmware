#pragma once

#include "esp_brookesia.hpp"
#include "esp_brookesia.hpp"
#include "lvgl.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string>
#include <vector>

class AppLocalMusic : public esp_brookesia::systems::phone::App {
public:
    AppLocalMusic();
    ~AppLocalMusic();
    void play_song_by_name(const char* name);

protected:
    bool run() override;
    bool back() override;
    bool close() override;

private:
    static void ui_event_cb(lv_event_t * e);
    static void audio_task(void *pvParameter);

    void update_play_button_text();
    void start_playback();
    void stop_playback();
    void load_playlist();
    void play_next();
    void play_prev();
    void update_volume(int change);

    std::vector<std::string> _playlist;
    int _current_song_index;
    int _volume;

    lv_obj_t * _bg_obj;
    lv_obj_t * _title_label;
    lv_obj_t * _btn_play;
    lv_obj_t * _lbl_play;
    lv_obj_t * _btn_next;
    lv_obj_t * _btn_prev;
    lv_obj_t * _btn_vol_up;
    lv_obj_t * _btn_vol_down;
    lv_obj_t * _vol_label;
    lv_obj_t * _status_label;

    bool _is_playing;
    bool _is_app_closed;
    bool _song_changed;
    TaskHandle_t _audio_task_handle;
};

#ifdef __cplusplus
extern "C" {
#endif
void app_local_music_play_from_ble(const char* song_name);
#ifdef __cplusplus
}
#endif
