#include "app_voice_recorder.hpp"
#include "esp_log.h"
#include <sys/stat.h>

#define TAG "VoiceRec"
#define TEST_FILE "/sdcard/test.wav"

extern esp_codec_dev_handle_t bsp_audio_codec_microphone_init(void);
extern esp_codec_dev_handle_t spk_codec_dev;

AppVoiceRecorder::AppVoiceRecorder() 
    : esp_brookesia::systems::phone::App("Ses Kayit", LV_SYMBOL_AUDIO, true),
      _is_recording(false), _is_playing(false), _is_app_closed(true), 
      _task_handle(NULL), _mic_codec(NULL) {
}

AppVoiceRecorder::~AppVoiceRecorder() {}

bool AppVoiceRecorder::run() {
    _is_app_closed = false;
    lv_obj_t* scr = lv_scr_act();
    lv_obj_clean(scr);
    
    // UI: Status Label
    _lbl_status = lv_label_create(scr);
    lv_label_set_text(_lbl_status, "Hazir");
    lv_obj_align(_lbl_status, LV_ALIGN_TOP_MID, 0, 80);
    lv_obj_set_style_text_color(_lbl_status, lv_color_white(), 0);

    // UI: Record Button
    _btn_record = lv_btn_create(scr);
    lv_obj_set_size(_btn_record, 200, 60);
    lv_obj_align(_btn_record, LV_ALIGN_CENTER, 0, -40);
    lv_obj_add_event_cb(_btn_record, btn_record_cb, LV_EVENT_CLICKED, this);
    lv_obj_t* lbl_rec = lv_label_create(_btn_record);
    lv_label_set_text(lbl_rec, "KAYDET");
    lv_obj_center(lbl_rec);

    // UI: Play Button
    _btn_play = lv_btn_create(scr);
    lv_obj_set_size(_btn_play, 200, 60);
    lv_obj_align(_btn_play, LV_ALIGN_CENTER, 0, 40);
    lv_obj_add_event_cb(_btn_play, btn_play_cb, LV_EVENT_CLICKED, this);
    lv_obj_t* lbl_play = lv_label_create(_btn_play);
    lv_label_set_text(lbl_play, "OYNAT");
    lv_obj_center(lbl_play);

    _mic_codec = bsp_audio_codec_microphone_init();
    update_ui();
    return true;
}

bool AppVoiceRecorder::back() {
    return true;
}

bool AppVoiceRecorder::close() {
    _is_app_closed = true;
    _is_recording = false;
    _is_playing = false;
    if (_mic_codec) {
        esp_codec_dev_close(_mic_codec);
        _mic_codec = NULL;
    }
    return true;
}

void AppVoiceRecorder::update_ui() {
    if (_is_recording) {
        lv_label_set_text(_lbl_status, "Kaydediliyor...");
        lv_obj_set_style_bg_color(_btn_record, lv_color_hex(0xFF0000), 0);
        lv_obj_add_state(_btn_play, LV_STATE_DISABLED);
    } else if (_is_playing) {
        lv_label_set_text(_lbl_status, "Oynatiliyor...");
        lv_obj_set_style_bg_color(_btn_play, lv_color_hex(0x00FF00), 0);
        lv_obj_add_state(_btn_record, LV_STATE_DISABLED);
    } else {
        lv_label_set_text(_lbl_status, "Hazir");
        lv_obj_set_style_bg_color(_btn_record, lv_palette_main(LV_PALETTE_BLUE), 0);
        lv_obj_set_style_bg_color(_btn_play, lv_palette_main(LV_PALETTE_BLUE), 0);
        lv_obj_clear_state(_btn_play, LV_STATE_DISABLED);
        lv_obj_clear_state(_btn_record, LV_STATE_DISABLED);
    }
}

void AppVoiceRecorder::btn_record_cb(lv_event_t *e) {
    AppVoiceRecorder *self = (AppVoiceRecorder *)lv_event_get_user_data(e);
    if (self->_is_recording) {
        self->_is_recording = false;
    } else if (!self->_is_playing) {
        self->_is_recording = true;
        self->update_ui();
        xTaskCreate(audio_task, "audio_task", 8192, self, 3, &self->_task_handle);
    }
}

void AppVoiceRecorder::btn_play_cb(lv_event_t *e) {
    AppVoiceRecorder *self = (AppVoiceRecorder *)lv_event_get_user_data(e);
    if (self->_is_playing) {
        self->_is_playing = false;
    } else if (!self->_is_recording) {
        self->_is_playing = true;
        self->update_ui();
        xTaskCreate(audio_task, "audio_task", 8192, self, 3, &self->_task_handle);
    }
}

static void write_wav_header(FILE* f, uint32_t data_size) {
    uint32_t sample_rate = 22050;
    uint16_t num_channels = 1;
    uint16_t bits_per_sample = 16;
    uint32_t byte_rate = sample_rate * num_channels * (bits_per_sample / 8);
    uint32_t file_size = 36 + data_size;

    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f);
    fwrite(&file_size, 4, 1, f);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    uint32_t fmt_chunk_size = 16;
    fwrite(&fmt_chunk_size, 4, 1, f);
    uint16_t audio_format = 1;
    fwrite(&audio_format, 2, 1, f);
    fwrite(&num_channels, 2, 1, f);
    fwrite(&sample_rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f);
    uint16_t block_align = num_channels * (bits_per_sample / 8);
    fwrite(&block_align, 2, 1, f);
    fwrite(&bits_per_sample, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&data_size, 4, 1, f);
}

void AppVoiceRecorder::audio_task(void *pvParameter) {
    AppVoiceRecorder *self = (AppVoiceRecorder *)pvParameter;
    
    uint8_t *buffer = (uint8_t *)malloc(1024);
    
    if (self->_is_recording) {
        ESP_LOGI(TAG, "RECORD: Basliyor");
        FILE *f = fopen(TEST_FILE, "wb");
        if (f) {
            write_wav_header(f, 0);
            
            esp_codec_dev_sample_info_t fs = {
                .bits_per_sample = 16,
                .channel = 1,
                .channel_mask = 0,
                .sample_rate = 22050,
                .mclk_multiple = 0,
            };
            esp_codec_dev_open(self->_mic_codec, &fs);
            esp_codec_dev_set_in_gain(self->_mic_codec, 30.0);
            
            uint32_t total_written = 0;
            while (self->_is_recording && !self->_is_app_closed) {
                int ret = esp_codec_dev_read(self->_mic_codec, buffer, 1024);
                if (ret == 0) {
                    fwrite(buffer, 1, 1024, f);
                    total_written += 1024;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            
            write_wav_header(f, total_written);
            fclose(f);
            esp_codec_dev_close(self->_mic_codec);
            ESP_LOGI(TAG, "RECORD: Bitti. %lu byte yazildi.", total_written);
        }
        self->_is_recording = false;
        
    } else if (self->_is_playing) {
        ESP_LOGI(TAG, "PLAY: Basliyor");
        FILE *f = fopen(TEST_FILE, "rb");
        if (f) {
            fseek(f, 44, SEEK_SET);
            
            esp_codec_dev_sample_info_t fs = {
                .bits_per_sample = 16,
                .channel = 1,
                .channel_mask = 0,
                .sample_rate = 22050,
                .mclk_multiple = 0,
            };
            
            if (spk_codec_dev) {
                esp_codec_dev_open(spk_codec_dev, &fs);
                esp_codec_dev_set_out_vol(spk_codec_dev, 70);
                
                while (self->_is_playing && !self->_is_app_closed) {
                    size_t read_bytes = fread(buffer, 1, 1024, f);
                    if (read_bytes == 0) break;
                    
                    esp_codec_dev_write(spk_codec_dev, buffer, read_bytes);
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
                esp_codec_dev_close(spk_codec_dev);
            }
            fclose(f);
            ESP_LOGI(TAG, "PLAY: Bitti.");
        }
        self->_is_playing = false;
    }

    free(buffer);
    
    if (bsp_display_lock(100)) {
        self->update_ui();
        bsp_display_unlock();
    }
    
    self->_task_handle = NULL;
    vTaskDelete(NULL);
}
