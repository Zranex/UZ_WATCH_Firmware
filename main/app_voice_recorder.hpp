#pragma once

#include "esp_brookesia.hpp"
#include "lvgl.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string>
#include <vector>
#include "esp_codec_dev.h"

class AppVoiceRecorder : public esp_brookesia::systems::phone::App {
public:
    AppVoiceRecorder();
    ~AppVoiceRecorder();

protected:
    bool run() override;
    bool back() override;
    bool close() override;

private:
    // UI event callbacks
    static void btn_record_cb(lv_event_t *e);
    static void btn_play_cb(lv_event_t *e);
    static void btn_delete_cb(lv_event_t *e);
    static void list_item_cb(lv_event_t *e);

    // Background tasks
    static void record_task(void *pvParameter);
    static void playback_task(void *pvParameter);

    // Internal
    void start_recording();
    void stop_recording();
    void start_playback();
    void stop_playback();
    void load_recordings_list();
    void update_ui_state();
    void update_timer_label();
    std::string generate_filename();

    // UI elements
    lv_obj_t *_bg_obj;
    lv_obj_t *_title_label;
    lv_obj_t *_timer_label;
    lv_obj_t *_btn_record;
    lv_obj_t *_lbl_record;
    lv_obj_t *_btn_play;
    lv_obj_t *_lbl_play;
    lv_obj_t *_btn_delete;
    lv_obj_t *_lbl_delete;
    lv_obj_t *_recordings_list;
    lv_obj_t *_status_label;

    // State
    bool _is_recording;
    bool _is_playing;
    bool _is_app_closed;
    uint32_t _record_start_tick;
    int _selected_recording_index;

    // Task handles
    TaskHandle_t _record_task_handle;
    TaskHandle_t _playback_task_handle;

    // Codec
    esp_codec_dev_handle_t _mic_codec_dev;

    // Recordings
    std::vector<std::string> _recordings;
};
