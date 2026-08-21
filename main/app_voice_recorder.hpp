#pragma once

#include "esp_brookesia.hpp"
#include <string>
#include <vector>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_codec_dev.h"
#include "bsp/esp-bsp.h"

class AppVoiceRecorder : public esp_brookesia::systems::phone::App {
public:
    AppVoiceRecorder();
    ~AppVoiceRecorder();

protected:
    bool run() override;
    bool back() override;
    bool close() override;

private:
    lv_obj_t* _title_label;
    lv_obj_t* _timer_label;
    lv_obj_t* _btn_record;
    lv_obj_t* _btn_play;
    lv_obj_t* _btn_delete;
    lv_obj_t* _recordings_list;
    lv_obj_t* _status_label;

    bool _is_recording;
    bool _is_playing;
    bool _is_app_closed;
    uint32_t _record_start_tick;
    int _selected_index;

    TaskHandle_t _task_handle;
    esp_codec_dev_handle_t _mic_codec;
    std::vector<std::string> _recordings;

    static void btn_record_cb(lv_event_t *e);
    static void btn_play_cb(lv_event_t *e);
    static void btn_delete_cb(lv_event_t *e);
    static void list_item_cb(lv_event_t *e);
    static void audio_task(void *pvParameter);

    void update_ui();
    void update_timer();
    void load_list();
    std::string generate_filename();
};
