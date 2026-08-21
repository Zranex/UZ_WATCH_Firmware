#include "app_voice_recorder.hpp"
#include "esp_lib_utils.h"
#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include <errno.h>

extern esp_codec_dev_handle_t spk_codec_dev;
LV_FONT_DECLARE(font_cinzel_bold_36);

using namespace esp_brookesia;

// Recording parameters
#define RECORDER_SAMPLE_RATE    22050
#define RECORDER_CHANNELS       1
#define RECORDER_BITS           16
#define RECORDER_CHUNK_SIZE     1024
#define RECORDINGS_DIR          BSP_SD_MOUNT_POINT "/recordings"

// WAV header structure for writing
typedef struct {
    char     riff_header[4];    // "RIFF"
    uint32_t wav_size;          // File size - 8
    char     wave_header[4];    // "WAVE"
    char     fmt_header[4];     // "fmt "
    uint32_t fmt_chunk_size;    // 16
    uint16_t audio_format;     // 1 = PCM
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t sample_alignment;
    uint16_t bit_depth;
    char     data_header[4];    // "data"
    uint32_t data_bytes;        // Data size
} wav_header_write_t;

static void write_wav_header(FILE *f, uint32_t data_size) {
    wav_header_write_t hdr = {};
    memcpy(hdr.riff_header, "RIFF", 4);
    hdr.wav_size = data_size + 36; // 44 - 8
    memcpy(hdr.wave_header, "WAVE", 4);
    memcpy(hdr.fmt_header, "fmt ", 4);
    hdr.fmt_chunk_size = 16;
    hdr.audio_format = 1; // PCM
    hdr.num_channels = RECORDER_CHANNELS;
    hdr.sample_rate = RECORDER_SAMPLE_RATE;
    hdr.byte_rate = RECORDER_SAMPLE_RATE * RECORDER_CHANNELS * (RECORDER_BITS / 8);
    hdr.sample_alignment = RECORDER_CHANNELS * (RECORDER_BITS / 8);
    hdr.bit_depth = RECORDER_BITS;
    memcpy(hdr.data_header, "data", 4);
    hdr.data_bytes = data_size;
    fseek(f, 0, SEEK_SET);
    fwrite(&hdr, sizeof(wav_header_write_t), 1, f);
}

AppVoiceRecorder::AppVoiceRecorder() : systems::phone::App("Ses Kayit", nullptr, true) {
    _bg_obj = nullptr;
    _title_label = nullptr;
    _timer_label = nullptr;
    _btn_record = nullptr;
    _lbl_record = nullptr;
    _btn_play = nullptr;
    _lbl_play = nullptr;
    _btn_delete = nullptr;
    _lbl_delete = nullptr;
    _recordings_list = nullptr;
    _status_label = nullptr;
    _is_recording = false;
    _is_playing = false;
    _is_app_closed = true;
    _record_start_tick = 0;
    _selected_recording_index = -1;
    _record_task_handle = NULL;
    _playback_task_handle = NULL;
    _mic_codec_dev = NULL;
}

AppVoiceRecorder::~AppVoiceRecorder() {
    _is_app_closed = true;
    _is_recording = false;
    _is_playing = false;
}

std::string AppVoiceRecorder::generate_filename() {
    // Generate filename with rec_ prefix (saved to SD card root)
    char name[64];
    int count = (int)_recordings.size() + 1;
    snprintf(name, sizeof(name), "rec_%03d.wav", count);

    // Check if file exists, increment if needed
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", BSP_SD_MOUNT_POINT, name);
    while (access(path, F_OK) == 0) {
        count++;
        snprintf(name, sizeof(name), "rec_%03d.wav", count);
        snprintf(path, sizeof(path), "%s/%s", BSP_SD_MOUNT_POINT, name);
    }
    return std::string(name);
}

void AppVoiceRecorder::load_recordings_list() {
    _recordings.clear();

    // Scan SD card root for rec_*.wav files
    DIR *dir = opendir(BSP_SD_MOUNT_POINT);
    if (!dir) {
        ESP_UTILS_LOGE("Cannot open SD card root dir");
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG) {
            std::string fname(entry->d_name);
            
            // Convert to lowercase for checking
            std::string fname_lower = fname;
            for (char &c : fname_lower) {
                c = std::tolower(c);
            }
            
            // Only show rec_*.wav files that are valid (>= 44 bytes)
            if (fname_lower.size() > 4 && 
                fname_lower.substr(0, 4) == "rec_" && 
                fname_lower.substr(fname_lower.size() - 4) == ".wav") {
                
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

    // Sort alphabetically
    std::sort(_recordings.begin(), _recordings.end());
    ESP_UTILS_LOGI("Found %d recordings", (int)_recordings.size());
}

void AppVoiceRecorder::update_timer_label() {
    if (!_timer_label) return;
    if (_is_recording) {
        uint32_t elapsed_ms = (xTaskGetTickCount() - _record_start_tick) * portTICK_PERIOD_MS;
        uint32_t secs = elapsed_ms / 1000;
        uint32_t mins = secs / 60;
        secs %= 60;
        char buf[16];
        snprintf(buf, sizeof(buf), "%02lu:%02lu", (unsigned long)mins, (unsigned long)secs);
        lv_label_set_text(_timer_label, buf);
    } else {
        lv_label_set_text(_timer_label, "00:00");
    }
}

void AppVoiceRecorder::update_ui_state() {
    if (!_btn_record || !_btn_play || !_btn_delete) return;

    if (_is_recording) {
        lv_label_set_text(_lbl_record, LV_SYMBOL_STOP " Durdur");
        lv_obj_set_style_bg_color(_btn_record, lv_color_hex(0xCC0000), 0);
        lv_obj_add_state(_btn_play, LV_STATE_DISABLED);
        lv_obj_add_state(_btn_delete, LV_STATE_DISABLED);
    } else if (_is_playing) {
        lv_label_set_text(_lbl_record, LV_SYMBOL_AUDIO " Kaydet");
        lv_obj_set_style_bg_color(_btn_record, lv_color_hex(0x333333), 0);
        lv_obj_add_state(_btn_record, LV_STATE_DISABLED);
        lv_label_set_text(_lbl_play, LV_SYMBOL_STOP " Durdur");
        lv_obj_clear_state(_btn_delete, LV_STATE_DISABLED);
    } else {
        lv_label_set_text(_lbl_record, LV_SYMBOL_AUDIO " Kaydet");
        lv_obj_set_style_bg_color(_btn_record, lv_color_hex(0xCC0000), 0);
        lv_obj_clear_state(_btn_record, LV_STATE_DISABLED);
        lv_label_set_text(_lbl_play, LV_SYMBOL_PLAY " Oynat");
        if (_selected_recording_index >= 0) {
            lv_obj_clear_state(_btn_play, LV_STATE_DISABLED);
            lv_obj_clear_state(_btn_delete, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(_btn_play, LV_STATE_DISABLED);
            lv_obj_add_state(_btn_delete, LV_STATE_DISABLED);
        }
    }
}

// ─── Record Task ───
void AppVoiceRecorder::record_task(void *pvParameter) {
    AppVoiceRecorder *self = (AppVoiceRecorder *)pvParameter;

    // Initialize microphone codec
    self->_mic_codec_dev = bsp_audio_codec_microphone_init();
    if (!self->_mic_codec_dev) {
        ESP_UTILS_LOGE("Failed to init microphone codec!");
        if (bsp_display_lock(100)) {
            if (self->_status_label)
                lv_label_set_text(self->_status_label, "Mikrofon Hatasi!");
            bsp_display_unlock();
        }
        self->_is_recording = false;
        self->_record_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    // Open microphone for reading
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = RECORDER_BITS,
        .channel = RECORDER_CHANNELS,
        .channel_mask = 0,
        .sample_rate = RECORDER_SAMPLE_RATE,
        .mclk_multiple = 0,
    };
    esp_err_t ret = esp_codec_dev_open(self->_mic_codec_dev, &fs);
    if (ret != ESP_OK) {
        ESP_UTILS_LOGE("Failed to open mic codec: %d", ret);
        self->_is_recording = false;
        self->_record_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    // Set microphone gain
    esp_codec_dev_set_in_gain(self->_mic_codec_dev, 30.0);

    // Generate filename - save directly to SD card root with rec_ prefix
    std::string filename = self->generate_filename();
    char filepath[256];
    snprintf(filepath, sizeof(filepath), "%s/%s", BSP_SD_MOUNT_POINT, filename.c_str());

    ESP_UTILS_LOGI("Attempting to create file: %s", filepath);

    // Quick SD card write test
    char test_path[256];
    snprintf(test_path, sizeof(test_path), "%s/test.tmp", BSP_SD_MOUNT_POINT);
    FILE *test_f = fopen(test_path, "wb");
    if (test_f) {
        fwrite("OK", 1, 2, test_f);
        fclose(test_f);
        remove(test_path);
        ESP_UTILS_LOGI("SD card write test: OK");
    } else {
        ESP_UTILS_LOGE("SD card write test FAILED! errno=%d", errno);
    }

    FILE *wav_file = fopen(filepath, "wb");
    if (!wav_file) {
        ESP_UTILS_LOGE("Failed to create recording file: %s (errno=%d)", filepath, errno);
        esp_codec_dev_close(self->_mic_codec_dev);
        self->_mic_codec_dev = NULL;
        self->_is_recording = false;
        if (bsp_display_lock(100)) {
            if (self->_status_label)
                lv_label_set_text(self->_status_label, "Dosya Olusturulamadi!");
            self->update_ui_state();
            bsp_display_unlock();
        }
        self->_record_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    // Write placeholder WAV header (will be updated at end)
    write_wav_header(wav_file, 0);

    ESP_UTILS_LOGI("Recording started: %s", filepath);

    // Update status label
    if (bsp_display_lock(100)) {
        if (self->_status_label)
            lv_label_set_text(self->_status_label, filename.c_str());
        bsp_display_unlock();
    }

    // Record loop
    uint8_t *buffer = (uint8_t *)heap_caps_malloc(RECORDER_CHUNK_SIZE, MALLOC_CAP_SPIRAM);
    if (!buffer) {
        buffer = (uint8_t *)malloc(RECORDER_CHUNK_SIZE);
    }

    uint32_t total_data_bytes = 0;
    uint32_t ui_update_counter = 0;
    int consecutive_read_errors = 0;

    ESP_UTILS_LOGI("Starting recording loop...");

    while (self->_is_recording && !self->_is_app_closed) {
        int ret = esp_codec_dev_read(self->_mic_codec_dev, buffer, RECORDER_CHUNK_SIZE);
        
        if (ret == 0) { // ESP_CODEC_DEV_OK
            consecutive_read_errors = 0;
            size_t written = fwrite(buffer, 1, RECORDER_CHUNK_SIZE, wav_file);
            if (written != RECORDER_CHUNK_SIZE) {
                ESP_UTILS_LOGE("fwrite failed! tried: %d, wrote: %d, errno: %d, ferror: %d", RECORDER_CHUNK_SIZE, written, errno, ferror(wav_file));
            }
            total_data_bytes += written;
        } else if (ret < 0) {
            consecutive_read_errors++;
            if (consecutive_read_errors > 10) {
                ESP_UTILS_LOGE("Too many read errors, aborting recording!");
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }// CRITICAL: Yield CPU to LVGL so touch/UI stays responsive
        vTaskDelay(pdMS_TO_TICKS(1));

        // Update timer UI every ~500ms
        ui_update_counter++;
        if (ui_update_counter >= 20) {
            ui_update_counter = 0;
            if (bsp_display_lock(10)) {
                self->update_timer_label();
                bsp_display_unlock();
            }
        }
    }

    free(buffer);

    // Finalize WAV header with correct data size
    write_wav_header(wav_file, total_data_bytes);
    fclose(wav_file);

    ESP_UTILS_LOGI("Recording stopped: %s (%lu bytes)", filepath, (unsigned long)total_data_bytes);

    // Close microphone
    esp_codec_dev_close(self->_mic_codec_dev);
    self->_mic_codec_dev = NULL;

    // Reload list and update UI
    self->load_recordings_list();
    if (bsp_display_lock(100)) {
        self->update_timer_label();

        // Rebuild list UI
        if (self->_recordings_list) {
            lv_obj_clean(self->_recordings_list);
            for (size_t i = 0; i < self->_recordings.size(); i++) {
                lv_obj_t *btn = lv_list_add_btn(self->_recordings_list, LV_SYMBOL_AUDIO, self->_recordings[i].c_str());
                lv_obj_set_style_bg_color(btn, lv_color_hex(0x222222), 0);
                lv_obj_set_style_text_color(btn, lv_color_hex(0xFFFFFF), 0);
                lv_obj_add_event_cb(btn, list_item_cb, LV_EVENT_CLICKED, self);
                lv_obj_set_user_data(btn, (void *)(intptr_t)i);
            }
        }

        char buf[64];
        snprintf(buf, sizeof(buf), "Kaydedildi: %s", filename.c_str());
        if (self->_status_label)
            lv_label_set_text(self->_status_label, buf);

        self->update_ui_state();
        bsp_display_unlock();
    }

    self->_record_task_handle = NULL;
    vTaskDelete(NULL);
}

// ─── Playback Task ───
void AppVoiceRecorder::playback_task(void *pvParameter) {
    AppVoiceRecorder *self = (AppVoiceRecorder *)pvParameter;

    if (self->_selected_recording_index < 0 || 
        self->_selected_recording_index >= (int)self->_recordings.size()) {
        self->_is_playing = false;
        self->_playback_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    std::string filename = self->_recordings[self->_selected_recording_index];
    char filepath[256];
    snprintf(filepath, sizeof(filepath), "%s/%s", BSP_SD_MOUNT_POINT, filename.c_str());

    FILE *wav_file = fopen(filepath, "rb");
    if (!wav_file) {
        ESP_UTILS_LOGE("Failed to open recording: %s", filepath);
        self->_is_playing = false;
        self->_playback_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    // Read WAV header to get sample rate etc.
    wav_header_write_t hdr;
    if (fread(&hdr, sizeof(wav_header_write_t), 1, wav_file) != 1) {
        ESP_UTILS_LOGE("Failed to read WAV header");
        fclose(wav_file);
        self->_is_playing = false;
        self->_playback_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    // Verify RIFF header
    if (memcmp(hdr.riff_header, "RIFF", 4) != 0 || memcmp(hdr.wave_header, "WAVE", 4) != 0) {
        ESP_UTILS_LOGE("Invalid WAV file");
        fclose(wav_file);
        self->_is_playing = false;
        self->_playback_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_UTILS_LOGI("Playing: %s (rate=%lu, ch=%d, bits=%d)",
        filename.c_str(), (unsigned long)hdr.sample_rate, hdr.num_channels, hdr.bit_depth);

    if (hdr.sample_rate == 0 || hdr.num_channels == 0 || hdr.bit_depth == 0) {
        ESP_UTILS_LOGE("Corrupted WAV header, aborting playback");
        fclose(wav_file);
        self->_is_playing = false;
        self->_playback_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    // Open speaker codec with the file's sample info
    extern esp_codec_dev_handle_t spk_codec_dev;
    if (!spk_codec_dev) {
        ESP_UTILS_LOGE("Speaker codec not initialized");
        fclose(wav_file);
        self->_is_playing = false;
        self->_playback_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = (uint8_t)hdr.bit_depth,
        .channel = (uint8_t)hdr.num_channels,
        .channel_mask = 0,
        .sample_rate = hdr.sample_rate,
        .mclk_multiple = 0,
    };
    
    esp_err_t ret = esp_codec_dev_open(spk_codec_dev, &fs);
    if (ret != ESP_OK) {
        ESP_UTILS_LOGE("esp_codec_dev_open failed: %d", ret);
        fclose(wav_file);
        self->_is_playing = false;
        self->_playback_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }
    
    esp_codec_dev_set_out_vol(spk_codec_dev, 60);

    // Playback loop
    uint8_t *buffer = (uint8_t *)heap_caps_malloc(1024, MALLOC_CAP_SPIRAM);
    if (!buffer) {
        buffer = (uint8_t *)malloc(1024);
    }

    uint32_t remaining = hdr.data_bytes;
    while (self->_is_playing && !self->_is_app_closed && remaining > 0) {
        uint32_t to_read = (remaining > 1024) ? 1024 : remaining;
        size_t bytes_read = fread(buffer, 1, to_read, wav_file);
        if (bytes_read == 0) break;

        esp_codec_dev_write(spk_codec_dev, buffer, bytes_read);
        remaining -= bytes_read;
        
        // Prevent CPU starvation in case write is non-blocking or too fast
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    free(buffer);
    esp_codec_dev_close(spk_codec_dev);
    fclose(wav_file);

    ESP_UTILS_LOGI("Playback finished: %s", filename.c_str());

    self->_is_playing = false;

    if (bsp_display_lock(100)) {
        if (self->_status_label)
            lv_label_set_text(self->_status_label, "Oynatma Bitti");
        self->update_ui_state();
        bsp_display_unlock();
    }

    self->_playback_task_handle = NULL;
    vTaskDelete(NULL);
}

void AppVoiceRecorder::start_recording() {
    if (_is_recording || _is_playing || _record_task_handle != NULL) return;

    _is_recording = true;
    _record_start_tick = xTaskGetTickCount();
    update_ui_state();

    xTaskCreate(record_task, "voice_rec_task", 16384, this, 2, &_record_task_handle);
}

void AppVoiceRecorder::stop_recording() {
    _is_recording = false;
    // Task will exit gracefully and clean up
}

void AppVoiceRecorder::start_playback() {
    if (_is_recording || _is_playing) return;
    if (_selected_recording_index < 0) return;

    std::string filename = _recordings[_selected_recording_index];
    char filepath[256];
    snprintf(filepath, sizeof(filepath), "%s/%s", BSP_SD_MOUNT_POINT, filename.c_str());

    // Abort early if file is too small (e.g., 0-byte recordings)
    struct stat st;
    if (stat(filepath, &st) != 0 || st.st_size < 44) {
        ESP_UTILS_LOGE("File %s is invalid or 0 bytes. Aborting playback.", filepath);
        if (_status_label) lv_label_set_text(_status_label, "Dosya Bozuk!");
        return;
    }

    _is_playing = true;
    update_ui_state();

    xTaskCreate(playback_task, "voice_play_task", 16384, this, 2, &_playback_task_handle);
}

void AppVoiceRecorder::stop_playback() {
    _is_playing = false;
    // Task will exit gracefully
}

// ─── UI Callbacks ───
void AppVoiceRecorder::btn_record_cb(lv_event_t *e) {
    AppVoiceRecorder *self = (AppVoiceRecorder *)lv_event_get_user_data(e);
    if (self->_is_recording) {
        self->stop_recording();
    } else {
        self->start_recording();
    }
}

void AppVoiceRecorder::btn_play_cb(lv_event_t *e) {
    AppVoiceRecorder *self = (AppVoiceRecorder *)lv_event_get_user_data(e);
    if (self->_is_playing) {
        self->stop_playback();
    } else {
        self->start_playback();
    }
}

void AppVoiceRecorder::btn_delete_cb(lv_event_t *e) {
    AppVoiceRecorder *self = (AppVoiceRecorder *)lv_event_get_user_data(e);
    if (self->_selected_recording_index < 0 || self->_is_recording || self->_is_playing) return;

    std::string filename = self->_recordings[self->_selected_recording_index];
    char filepath[256];
    snprintf(filepath, sizeof(filepath), "%s/%s", BSP_SD_MOUNT_POINT, filename.c_str());

    if (remove(filepath) == 0) {
        ESP_UTILS_LOGI("Deleted: %s", filepath);
    } else {
        ESP_UTILS_LOGE("Failed to delete: %s", filepath);
    }

    self->_selected_recording_index = -1;
    self->load_recordings_list();

    // Rebuild list UI
    if (self->_recordings_list) {
        lv_obj_clean(self->_recordings_list);
        for (size_t i = 0; i < self->_recordings.size(); i++) {
            lv_obj_t *btn = lv_list_add_btn(self->_recordings_list, LV_SYMBOL_AUDIO, self->_recordings[i].c_str());
            lv_obj_set_style_bg_color(btn, lv_color_hex(0x222222), 0);
            lv_obj_set_style_text_color(btn, lv_color_hex(0xFFFFFF), 0);
            lv_obj_add_event_cb(btn, list_item_cb, LV_EVENT_CLICKED, self);
            lv_obj_set_user_data(btn, (void *)(intptr_t)i);
        }
    }

    if (self->_status_label)
        lv_label_set_text(self->_status_label, "Silindi");
    self->update_ui_state();
}

void AppVoiceRecorder::list_item_cb(lv_event_t *e) {
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    AppVoiceRecorder *self = (AppVoiceRecorder *)lv_event_get_user_data(e);
    int index = (int)(intptr_t)lv_obj_get_user_data(btn);

    self->_selected_recording_index = index;

    // Highlight selected item
    if (self->_recordings_list) {
        uint32_t child_cnt = lv_obj_get_child_cnt(self->_recordings_list);
        for (uint32_t i = 0; i < child_cnt; i++) {
            lv_obj_t *child = lv_obj_get_child(self->_recordings_list, i);
            if ((int)i == index) {
                lv_obj_set_style_bg_color(child, lv_color_hex(0xCC0000), 0);
            } else {
                lv_obj_set_style_bg_color(child, lv_color_hex(0x222222), 0);
            }
        }
    }

    if (self->_status_label && index >= 0 && index < (int)self->_recordings.size()) {
        lv_label_set_text(self->_status_label, self->_recordings[index].c_str());
    }
    self->update_ui_state();
}

// ─── App Lifecycle ───
bool AppVoiceRecorder::run() {
    _is_app_closed = false;
    load_recordings_list();

    lv_obj_t *screen = lv_scr_act();

    // Background
    _bg_obj = lv_obj_create(screen);
    lv_obj_set_size(_bg_obj, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(_bg_obj, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(_bg_obj, 0, 0);
    lv_obj_set_style_pad_all(_bg_obj, 8, 0);
    lv_obj_set_flex_flow(_bg_obj, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(_bg_obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(_bg_obj, 6, 0);

    // Title
    _title_label = lv_label_create(_bg_obj);
    lv_label_set_text(_title_label, "Ses Kayit");
    lv_obj_set_style_text_font(_title_label, &font_cinzel_bold_36, 0);
    lv_obj_set_style_text_color(_title_label, lv_color_hex(0xCC0000), 0);

    // Timer display
    _timer_label = lv_label_create(_bg_obj);
    lv_label_set_text(_timer_label, "00:00");
    lv_obj_set_style_text_font(_timer_label, &font_cinzel_bold_36, 0);
    lv_obj_set_style_text_color(_timer_label, lv_color_hex(0xFFFFFF), 0);

    // ─── Buttons row ───
    lv_obj_t *btn_row = lv_obj_create(_bg_obj);
    lv_obj_set_size(btn_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_style_pad_all(btn_row, 0, 0);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Record button
    _btn_record = lv_btn_create(btn_row);
    lv_obj_set_size(_btn_record, 110, 50);
    lv_obj_set_style_bg_color(_btn_record, lv_color_hex(0xCC0000), 0);
    lv_obj_set_style_radius(_btn_record, 12, 0);
    _lbl_record = lv_label_create(_btn_record);
    lv_label_set_text(_lbl_record, LV_SYMBOL_AUDIO " Kaydet");
    lv_obj_set_style_text_color(_lbl_record, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(_lbl_record);
    lv_obj_add_event_cb(_btn_record, btn_record_cb, LV_EVENT_CLICKED, this);

    // Play button
    _btn_play = lv_btn_create(btn_row);
    lv_obj_set_size(_btn_play, 110, 50);
    lv_obj_set_style_bg_color(_btn_play, lv_color_hex(0x333333), 0);
    lv_obj_set_style_radius(_btn_play, 12, 0);
    _lbl_play = lv_label_create(_btn_play);
    lv_label_set_text(_lbl_play, LV_SYMBOL_PLAY " Oynat");
    lv_obj_set_style_text_color(_lbl_play, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(_lbl_play);
    lv_obj_add_event_cb(_btn_play, btn_play_cb, LV_EVENT_CLICKED, this);

    // Delete button row (separate)
    lv_obj_t *del_row = lv_obj_create(_bg_obj);
    lv_obj_set_size(del_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(del_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(del_row, 0, 0);
    lv_obj_set_style_pad_all(del_row, 0, 0);
    lv_obj_set_flex_flow(del_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(del_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    _btn_delete = lv_btn_create(del_row);
    lv_obj_set_size(_btn_delete, 130, 40);
    lv_obj_set_style_bg_color(_btn_delete, lv_color_hex(0x660000), 0);
    lv_obj_set_style_radius(_btn_delete, 8, 0);
    _lbl_delete = lv_label_create(_btn_delete);
    lv_label_set_text(_lbl_delete, LV_SYMBOL_TRASH " Sil");
    lv_obj_set_style_text_color(_lbl_delete, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(_lbl_delete);
    lv_obj_add_event_cb(_btn_delete, btn_delete_cb, LV_EVENT_CLICKED, this);

    // Status label
    _status_label = lv_label_create(_bg_obj);
    lv_label_set_text(_status_label, "Hazir");
    lv_obj_set_style_text_color(_status_label, lv_color_hex(0x888888), 0);

    // ─── Recordings list ───
    _recordings_list = lv_list_create(_bg_obj);
    lv_obj_set_size(_recordings_list, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(_recordings_list, 200, 0);
    lv_obj_set_style_bg_color(_recordings_list, lv_color_hex(0x111111), 0);
    lv_obj_set_style_border_color(_recordings_list, lv_color_hex(0x333333), 0);
    lv_obj_set_style_border_width(_recordings_list, 1, 0);
    lv_obj_set_style_radius(_recordings_list, 8, 0);
    lv_obj_set_flex_grow(_recordings_list, 1);

    // Populate list
    for (size_t i = 0; i < _recordings.size(); i++) {
        lv_obj_t *btn = lv_list_add_btn(_recordings_list, LV_SYMBOL_AUDIO, _recordings[i].c_str());
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x222222), 0);
        lv_obj_set_style_text_color(btn, lv_color_hex(0xFFFFFF), 0);
        lv_obj_add_event_cb(btn, list_item_cb, LV_EVENT_CLICKED, this);
        lv_obj_set_user_data(btn, (void *)(intptr_t)i);
    }

    update_ui_state();

    return true;
}

bool AppVoiceRecorder::back() {
    // Stop any active recording/playback gracefully
    _is_recording = false;
    _is_playing = false;
    return true;
}

bool AppVoiceRecorder::close() {
    _is_app_closed = true;
    _is_recording = false;
    _is_playing = false;

    // Wait for tasks to finish
    if (_record_task_handle) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (_playback_task_handle) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    return true;
}
