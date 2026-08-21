#pragma once

#include "esp_brookesia.hpp"
#include <string>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_codec_dev.h"
#include "bsp/esp-bsp.h"
#include <stdio.h>

class AppVoiceRecorder : public esp_brookesia::systems::phone::App {
public:
    AppVoiceRecorder();
    ~AppVoiceRecorder();

protected:
    bool run() override;
    bool back() override;
    bool close() override;

private:
    lv_obj_t* _btn_record;
    lv_obj_t* _btn_play;
    lv_obj_t* _lbl_status;

    bool _is_recording;
    bool _is_playing;
    bool _is_app_closed;

    TaskHandle_t _task_handle;
    esp_codec_dev_handle_t _mic_codec;

    static void btn_record_cb(lv_event_t *e);
    static void btn_play_cb(lv_event_t *e);
    static void audio_task(void *pvParameter);

    void update_ui();
};
