#include "app_voice_recorder.hpp"
extern const lv_image_dsc_t icon_voice_recorder;
#include "esp_log.h"
#include <sys/stat.h>
#include <dirent.h>
#include <algorithm>
#include <ctype.h>

#define TAG "VoiceRec"

extern esp_codec_dev_handle_t bsp_audio_codec_microphone_init(void);
extern esp_codec_dev_handle_t spk_codec_dev;

AppVoiceRecorder::AppVoiceRecorder() 
    : esp_brookesia::systems::phone::App("Ses Kaydi", &icon_voice_recorder, true),
      _is_recording(false), _is_playing(false), _is_app_closed(true), 
      _record_start_tick(0), _selected_index(-1), _task_handle(NULL), _mic_codec(NULL) {
}

AppVoiceRecorder::~AppVoiceRecorder() {}

bool AppVoiceRecorder::run() {
    _is_app_closed = false;
    _selected_index = -1;
    lv_obj_t* scr = lv_scr_act();
    lv_obj_clean(scr);

    
    // UI: Title
    _title_label = lv_label_create(scr);
    lv_label_set_text(_title_label, "Ses Kaydedici");
    lv_obj_align(_title_label, LV_ALIGN_TOP_MID, 0, 10);

    // UI: Timer
    _timer_label = lv_label_create(scr);
    lv_label_set_text(_timer_label, "00:00");
    lv_obj_align(_timer_label, LV_ALIGN_TOP_MID, 0, 40);

    // UI: Status
    _status_label = lv_label_create(scr);
    lv_label_set_text(_status_label, "Hazir");
    lv_obj_align(_status_label, LV_ALIGN_TOP_MID, 0, 65);

    // UI: Buttons Row
    lv_obj_t* btn_row = lv_obj_create(scr);
    lv_obj_set_size(btn_row, LV_PCT(100), 80);
    lv_obj_align(btn_row, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_opa(btn_row, 0, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Record Button
    _btn_record = lv_btn_create(btn_row);
    lv_obj_set_size(_btn_record, 80, 60);
    lv_obj_add_event_cb(_btn_record, btn_record_cb, LV_EVENT_CLICKED, this);
    lv_obj_t* lbl_rec = lv_label_create(_btn_record);
    lv_label_set_text(lbl_rec, LV_SYMBOL_AUDIO " Kayit");
    lv_obj_center(lbl_rec);

    // Play Button
    _btn_play = lv_btn_create(btn_row);
    lv_obj_set_size(_btn_play, 80, 60);
    lv_obj_add_event_cb(_btn_play, btn_play_cb, LV_EVENT_CLICKED, this);
    lv_obj_t* lbl_play = lv_label_create(_btn_play);
    lv_label_set_text(lbl_play, LV_SYMBOL_PLAY " Oynat");
    lv_obj_center(lbl_play);

    // Delete Button
    _btn_delete = lv_btn_create(btn_row);
    lv_obj_set_size(_btn_delete, 80, 60);
    lv_obj_add_event_cb(_btn_delete, btn_delete_cb, LV_EVENT_CLICKED, this);
    lv_obj_t* lbl_del = lv_label_create(_btn_delete);
    lv_label_set_text(lbl_del, LV_SYMBOL_TRASH " Sil");
    lv_obj_center(lbl_del);

    // UI: List
    _recordings_list = lv_list_create(scr);
    lv_obj_set_size(_recordings_list, LV_PCT(90), 180);
    lv_obj_align(_recordings_list, LV_ALIGN_CENTER, 0, 0);

    _mic_codec = bsp_audio_codec_microphone_init();
    load_list();
    update_ui();
    return true;
}

bool AppVoiceRecorder::back() {
    return true;
}

bool AppVoiceRecorder::close() {
    _title_label = nullptr;
    _timer_label = nullptr;
    _btn_record = nullptr;
    _btn_play = nullptr;
    _btn_delete = nullptr;
    _recordings_list = nullptr;
    _status_label = nullptr;

    _is_app_closed = true;
    _is_recording = false;
    _is_playing = false;
    if (_mic_codec) {
        esp_codec_dev_close(_mic_codec);
        _mic_codec = NULL;
    }
    return true;
}

std::string AppVoiceRecorder::generate_filename() {
    int max_num = 0;
    for (const auto& fname : _recordings) {
        std::string fname_lower = fname;
        for (char &c : fname_lower) c = std::tolower(c);
        if (fname_lower.size() >= 11 && fname_lower.substr(0, 4) == "rec_") {
            int num = atoi(fname_lower.substr(4, 3).c_str());
            if (num > max_num) max_num = num;
        }
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "rec_%03d.wav", max_num + 1);
    return std::string(buf);
}

void AppVoiceRecorder::load_list() {
    _recordings.clear();
    DIR *dir = opendir(BSP_SD_MOUNT_POINT);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG) {
            std::string fname(entry->d_name);
            std::string fname_lower = fname;
            for (char &c : fname_lower) c = std::tolower(c);
            
            if (fname_lower.size() > 4 && fname_lower.substr(0, 4) == "rec_" && fname_lower.substr(fname_lower.size() - 4) == ".wav") {
                char fpath[256];
                snprintf(fpath, sizeof(fpath), "%s/%s", BSP_SD_MOUNT_POINT, fname.c_str());
                struct stat st;
                if (stat(fpath, &st) == 0 && st.st_size >= 44) {
                    _recordings.push_back(fname);
                }
            }
        }
    }
    closedir(dir);
    std::sort(_recordings.begin(), _recordings.end());

    if (_recordings_list) {
        lv_obj_clean(_recordings_list);
        for (size_t i = 0; i < _recordings.size(); i++) {
            lv_obj_t *btn = lv_list_add_btn(_recordings_list, LV_SYMBOL_AUDIO, _recordings[i].c_str());
            lv_obj_add_event_cb(btn, list_item_cb, LV_EVENT_CLICKED, this);
            lv_obj_set_user_data(btn, (void *)(intptr_t)i);
        }
    }
}

void AppVoiceRecorder::update_ui() {
    if (_is_recording) {
        lv_label_set_text(_status_label, "Kaydediliyor...");
        lv_obj_set_style_bg_color(_btn_record, lv_color_hex(0xFF0000), 0);
        lv_obj_add_state(_btn_play, LV_STATE_DISABLED);
        lv_obj_add_state(_btn_delete, LV_STATE_DISABLED);
    } else if (_is_playing) {
        lv_label_set_text(_status_label, "Oynatiliyor...");
        lv_obj_set_style_bg_color(_btn_play, lv_color_hex(0x00FF00), 0);
        lv_obj_add_state(_btn_record, LV_STATE_DISABLED);
        lv_obj_add_state(_btn_delete, LV_STATE_DISABLED);
    } else {
        lv_label_set_text(_status_label, _selected_index >= 0 ? "Hazir (Secili var)" : "Hazir");
        lv_obj_set_style_bg_color(_btn_record, lv_palette_main(LV_PALETTE_BLUE), 0);
        lv_obj_set_style_bg_color(_btn_play, lv_palette_main(LV_PALETTE_BLUE), 0);
        lv_obj_clear_state(_btn_play, LV_STATE_DISABLED);
        lv_obj_clear_state(_btn_record, LV_STATE_DISABLED);
        
        if (_selected_index >= 0) {
            lv_obj_clear_state(_btn_delete, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(_btn_delete, LV_STATE_DISABLED);
        }
    }
}

void AppVoiceRecorder::update_timer() {
    if (_is_recording && _record_start_tick > 0) {
        uint32_t elapsed_sec = (xTaskGetTickCount() - _record_start_tick) * portTICK_PERIOD_MS / 1000;
        char buf[16];
        snprintf(buf, sizeof(buf), "%02lu:%02lu", elapsed_sec / 60, elapsed_sec % 60);
        lv_label_set_text(_timer_label, buf);
    }
}

void AppVoiceRecorder::btn_record_cb(lv_event_t *e) {
    AppVoiceRecorder *self = (AppVoiceRecorder *)lv_event_get_user_data(e);
    if (self->_is_recording) {
        self->_is_recording = false;
    } else if (!self->_is_playing) {
        self->_is_recording = true;
        self->_record_start_tick = xTaskGetTickCount();
        self->update_ui();
        xTaskCreate(audio_task, "audio_task", 16384, self, 3, &self->_task_handle);
    }
}

void AppVoiceRecorder::btn_play_cb(lv_event_t *e) {
    AppVoiceRecorder *self = (AppVoiceRecorder *)lv_event_get_user_data(e);
    if (self->_is_playing) {
        self->_is_playing = false;
    } else if (!self->_is_recording && self->_selected_index >= 0 && self->_selected_index < (int)self->_recordings.size()) {
        self->_is_playing = true;
        self->update_ui();
        xTaskCreate(audio_task, "audio_task", 16384, self, 3, &self->_task_handle);
    }
}

void AppVoiceRecorder::btn_delete_cb(lv_event_t *e) {
    AppVoiceRecorder *self = (AppVoiceRecorder *)lv_event_get_user_data(e);
    if (self->_is_recording || self->_is_playing || self->_selected_index < 0) return;

    std::string filename = self->_recordings[self->_selected_index];
    char filepath[256];
    snprintf(filepath, sizeof(filepath), "%s/%s", BSP_SD_MOUNT_POINT, filename.c_str());

    remove(filepath);
    self->_selected_index = -1;
    self->load_list();
    self->update_ui();
}

void AppVoiceRecorder::list_item_cb(lv_event_t *e) {
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    AppVoiceRecorder *self = (AppVoiceRecorder *)lv_event_get_user_data(e);
    int index = (int)(intptr_t)lv_obj_get_user_data(btn);

    self->_selected_index = index;

    if (self->_recordings_list) {
        uint32_t child_cnt = lv_obj_get_child_cnt(self->_recordings_list);
        for (uint32_t i = 0; i < child_cnt; i++) {
            lv_obj_t *child = lv_obj_get_child(self->_recordings_list, i);
            if ((int)i == index) {
                lv_obj_set_style_bg_color(child, lv_color_hex(0xCC0000), 0); // K�rm�z� se�im
            } else {
                lv_obj_set_style_bg_color(child, lv_color_hex(0x222222), 0);
            }
        }
    }
    self->update_ui();
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
        std::string filename = self->generate_filename();
        char filepath[256];
        snprintf(filepath, sizeof(filepath), "%s/%s", BSP_SD_MOUNT_POINT, filename.c_str());
        
        FILE *f = fopen(filepath, "wb");
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
            uint32_t ui_update_counter = 0;
            while (self->_is_recording && !self->_is_app_closed) {
                int ret = esp_codec_dev_read(self->_mic_codec, buffer, 1024);
                if (ret == 0) {
                    fwrite(buffer, 1, 1024, f);
                    total_written += 1024;
                }
                
                ui_update_counter++;
                if (ui_update_counter > 50) {
                    ui_update_counter = 0;
                    if (bsp_display_lock(0)) {
                        self->update_timer();
                        bsp_display_unlock();
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            
            write_wav_header(f, total_written);
            fclose(f);
            esp_codec_dev_close(self->_mic_codec);
            
            if (bsp_display_lock(100)) {
                self->load_list();
                bsp_display_unlock();
            }
        }
        self->_is_recording = false;
        
    } else if (self->_is_playing) {
        std::string filename = self->_recordings[self->_selected_index];
        char filepath[256];
        snprintf(filepath, sizeof(filepath), "%s/%s", BSP_SD_MOUNT_POINT, filename.c_str());
        
        ESP_LOGI(TAG, "PLAY: Aciliyor -> %s", filepath);
        FILE *f = fopen(filepath, "rb");
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


