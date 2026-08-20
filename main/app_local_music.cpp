#include "app_local_music.hpp"
#include "esp_lib_utils.h"
#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"

typedef struct {
    char riff_header[4];
    uint32_t wav_size;
    char wave_header[4];
    char fmt_header[4];
    uint32_t fmt_chunk_size;
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t sample_alignment;
    uint16_t bit_depth;
    char data_header[4];
    uint32_t data_bytes;
} wav_header_t;

extern esp_codec_dev_handle_t spk_codec_dev;
LV_FONT_DECLARE(font_cinzel_bold_36);

using namespace esp_brookesia;

AppLocalMusic::AppLocalMusic() : systems::phone::App("Lokal Muzik", nullptr, true) {
    _bg_obj = nullptr;
    _title_label = nullptr;
    _btn_play = nullptr;
    _lbl_play = nullptr;
    _btn_next = nullptr;
    _btn_prev = nullptr;
    _btn_vol_up = nullptr;
    _btn_vol_down = nullptr;
    _vol_label = nullptr;
    _status_label = nullptr;
    _is_playing = false;
    _is_app_closed = true;
    _song_changed = false;
    _audio_task_handle = NULL;
    _current_song_index = 0;
    _volume = 60; // Default to 60 to prevent clipping/distortion
}

AppLocalMusic::~AppLocalMusic() {
    _is_app_closed = true;
}

void AppLocalMusic::load_playlist() {
    _playlist.clear();
    bool in_musics = true;
    DIR *d = opendir(BSP_SD_MOUNT_POINT "/musics");
    if (!d) {
        in_musics = false;
        d = opendir(BSP_SD_MOUNT_POINT);
        if (!d) return;
    }
    
    struct dirent *dir;
    while ((dir = readdir(d)) != NULL) {
        if (dir->d_type == DT_REG) {
            std::string name = dir->d_name;
            if (name.length() > 4) {
                std::string ext = name.substr(name.length() - 4);
                for (auto &c : ext) c = tolower(c);
                if (ext == ".wav") {
                    std::string path = std::string(BSP_SD_MOUNT_POINT) + (in_musics ? "/musics/" : "/") + name;
                    _playlist.push_back(path);
                }
            }
        }
    }
    closedir(d);
}

bool AppLocalMusic::run() {
    ESP_UTILS_LOGI("Starting AppLocalMusic");
    
    _is_app_closed = false;
    _is_playing = false;
    _song_changed = false;

    _bg_obj = lv_obj_create(lv_scr_act());
    lv_obj_set_size(_bg_obj, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(_bg_obj, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(_bg_obj, 0, 0);
    lv_obj_set_style_radius(_bg_obj, 0, 0);

    // Title
    _title_label = lv_label_create(_bg_obj);
    lv_label_set_text(_title_label, "SD Card Audio");
    lv_obj_align(_title_label, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_text_color(_title_label, lv_color_hex(0xFFD700), 0);
    lv_obj_set_style_text_font(_title_label, &font_cinzel_bold_36, 0);

    load_playlist();

    // Status label
    _status_label = lv_label_create(_bg_obj);
    lv_obj_set_width(_status_label, 300);
    lv_label_set_long_mode(_status_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    if (_playlist.empty()) {
        lv_label_set_text(_status_label, "Ready. No .wav found");
    } else {
        lv_label_set_text_fmt(_status_label, "Found %d songs", _playlist.size());
    }
    lv_obj_align(_status_label, LV_ALIGN_TOP_MID, 0, 90);
    lv_obj_set_style_text_color(_status_label, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_style_text_align(_status_label, LV_TEXT_ALIGN_CENTER, 0);
    
    // Play/Pause Button
    _btn_play = lv_btn_create(_bg_obj);
    lv_obj_set_size(_btn_play, 100, 100);
    lv_obj_align(_btn_play, LV_ALIGN_CENTER, 0, 30);
    lv_obj_set_style_radius(_btn_play, 50, 0);
    
    _lbl_play = lv_label_create(_btn_play);
    lv_label_set_text(_lbl_play, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_font(_lbl_play, &lv_font_montserrat_48, 0);
    lv_obj_center(_lbl_play);
    lv_obj_add_event_cb(_btn_play, ui_event_cb, LV_EVENT_CLICKED, this);

    // Prev Button
    _btn_prev = lv_btn_create(_bg_obj);
    lv_obj_set_size(_btn_prev, 70, 70);
    lv_obj_align_to(_btn_prev, _btn_play, LV_ALIGN_OUT_LEFT_MID, -20, 0);
    lv_obj_set_style_radius(_btn_prev, 35, 0);
    lv_obj_set_style_bg_color(_btn_prev, lv_color_hex(0x333333), 0);
    lv_obj_t* lbl_prev = lv_label_create(_btn_prev);
    lv_label_set_text(lbl_prev, LV_SYMBOL_PREV);
    lv_obj_set_style_text_font(lbl_prev, &lv_font_montserrat_24, 0);
    lv_obj_center(lbl_prev);
    lv_obj_add_event_cb(_btn_prev, [](lv_event_t* e){
        AppLocalMusic* app = (AppLocalMusic*)lv_event_get_user_data(e);
        app->play_prev();
    }, LV_EVENT_CLICKED, this);

    // Next Button
    _btn_next = lv_btn_create(_bg_obj);
    lv_obj_set_size(_btn_next, 70, 70);
    lv_obj_align_to(_btn_next, _btn_play, LV_ALIGN_OUT_RIGHT_MID, 20, 0);
    lv_obj_set_style_radius(_btn_next, 35, 0);
    lv_obj_set_style_bg_color(_btn_next, lv_color_hex(0x333333), 0);
    lv_obj_t* lbl_next = lv_label_create(_btn_next);
    lv_label_set_text(lbl_next, LV_SYMBOL_NEXT);
    lv_obj_set_style_text_font(lbl_next, &lv_font_montserrat_24, 0);
    lv_obj_center(lbl_next);
    lv_obj_add_event_cb(_btn_next, [](lv_event_t* e){
        AppLocalMusic* app = (AppLocalMusic*)lv_event_get_user_data(e);
        app->play_next();
    }, LV_EVENT_CLICKED, this);

    // Volume Down Button
    _btn_vol_down = lv_btn_create(_bg_obj);
    lv_obj_set_size(_btn_vol_down, 60, 60);
    lv_obj_align(_btn_vol_down, LV_ALIGN_BOTTOM_LEFT, 40, -40);
    lv_obj_set_style_radius(_btn_vol_down, 30, 0);
    lv_obj_set_style_bg_color(_btn_vol_down, lv_color_hex(0x555555), 0);
    lv_obj_t* lbl_vd = lv_label_create(_btn_vol_down);
    lv_label_set_text(lbl_vd, LV_SYMBOL_VOLUME_MID);
    lv_obj_center(lbl_vd);
    lv_obj_add_event_cb(_btn_vol_down, [](lv_event_t* e){
        AppLocalMusic* app = (AppLocalMusic*)lv_event_get_user_data(e);
        app->update_volume(-10);
    }, LV_EVENT_CLICKED, this);

    // Volume Up Button
    _btn_vol_up = lv_btn_create(_bg_obj);
    lv_obj_set_size(_btn_vol_up, 60, 60);
    lv_obj_align(_btn_vol_up, LV_ALIGN_BOTTOM_RIGHT, -40, -40);
    lv_obj_set_style_radius(_btn_vol_up, 30, 0);
    lv_obj_set_style_bg_color(_btn_vol_up, lv_color_hex(0x555555), 0);
    lv_obj_t* lbl_vu = lv_label_create(_btn_vol_up);
    lv_label_set_text(lbl_vu, LV_SYMBOL_VOLUME_MAX);
    lv_obj_center(lbl_vu);
    lv_obj_add_event_cb(_btn_vol_up, [](lv_event_t* e){
        AppLocalMusic* app = (AppLocalMusic*)lv_event_get_user_data(e);
        app->update_volume(10);
    }, LV_EVENT_CLICKED, this);

    // Volume Label
    _vol_label = lv_label_create(_bg_obj);
    lv_label_set_text_fmt(_vol_label, "%d%%", _volume);
    lv_obj_align(_vol_label, LV_ALIGN_BOTTOM_MID, 0, -60);
    lv_obj_set_style_text_color(_vol_label, lv_color_hex(0xFFFFFF), 0);

    update_play_button_text();
    
    if (_audio_task_handle == NULL) {
        xTaskCreate(audio_task, "local_audio_task", 16384, this, 4, &_audio_task_handle);
    }
    return true;
}

void AppLocalMusic::update_volume(int change) {
    _volume += change;
    if (_volume > 100) _volume = 100;
    if (_volume < 0) _volume = 0;
    if (_vol_label) lv_label_set_text_fmt(_vol_label, "%d%%", _volume);
    if (spk_codec_dev) {
        esp_codec_dev_set_out_vol(spk_codec_dev, _volume);
    }
}

void AppLocalMusic::play_next() {
    if (_playlist.empty()) return;
    _current_song_index = (_current_song_index + 1) % _playlist.size();
    _song_changed = true;
    _is_playing = true;
    update_play_button_text();
}

void AppLocalMusic::play_prev() {
    if (_playlist.empty()) return;
    _current_song_index--;
    if (_current_song_index < 0) _current_song_index = _playlist.size() - 1;
    _song_changed = true;
    _is_playing = true;
    update_play_button_text();
}

bool AppLocalMusic::back() {
    return true;
}

bool AppLocalMusic::close() {
    if (_bg_obj != nullptr) {
        lv_obj_del(_bg_obj);
        _bg_obj = nullptr;
    }
    _is_app_closed = true;
    return true;
}

void AppLocalMusic::ui_event_cb(lv_event_t * e) {
    AppLocalMusic * app = (AppLocalMusic *)lv_event_get_user_data(e);
    if (!app) return;

    if (app->_is_playing) {
        app->stop_playback();
    } else {
        app->start_playback();
    }
}

void AppLocalMusic::update_play_button_text() {
    if (!_lbl_play || !_btn_play) return;
    
    if (_is_playing) {
        lv_label_set_text(_lbl_play, LV_SYMBOL_PAUSE);
        lv_obj_set_style_bg_color(_btn_play, lv_color_hex(0xFF4444), 0);
    } else {
        lv_label_set_text(_lbl_play, LV_SYMBOL_PLAY);
        lv_obj_set_style_bg_color(_btn_play, lv_color_hex(0x1DB954), 0);
    }
}

void AppLocalMusic::start_playback() {
    if (_playlist.empty()) {
        if (_status_label) lv_label_set_text(_status_label, "Error: No .wav found");
        return;
    }
    
    if (!_is_playing) {
        _is_playing = true;
        // If it's the first time playing ever, we trigger a song change to load the file
        // Or if the task hasn't loaded any song yet. We'll handle this in the task.
        if (!_song_changed) {
            // Signal the task to open the file if it hasn't
            _song_changed = true; 
        }
    }
    update_play_button_text();
}

void AppLocalMusic::stop_playback() {
    _is_playing = false;
    update_play_button_text();
    if (_status_label) lv_label_set_text(_status_label, "Paused");
}

void AppLocalMusic::audio_task(void *pvParameter) {
    AppLocalMusic * app = (AppLocalMusic *)pvParameter;
    FILE * f = NULL;
    bool codec_opened = false;
    
    const int CHUNK_SIZE = 16384; 
    uint8_t * buf = (uint8_t *)malloc(CHUNK_SIZE);

    while (!app->_is_app_closed) {
        
        // Handle song changes (Next/Prev or initial play)
        if (app->_song_changed) {
            app->_song_changed = false;
            
            if (f) {
                fclose(f);
                f = NULL;
            }
            if (codec_opened) {
                esp_codec_dev_close(spk_codec_dev);
                codec_opened = false;
            }
            
            std::string filepath = app->_playlist[app->_current_song_index];
            f = fopen(filepath.c_str(), "rb");
            if (f != NULL) {
                char riff[4];
                uint32_t total_size;
                char wave[4];
                if (fread(riff, 1, 4, f) == 4 && strncmp(riff, "RIFF", 4) == 0 &&
                    fread(&total_size, 1, 4, f) == 4 &&
                    fread(wave, 1, 4, f) == 4 && strncmp(wave, "WAVE", 4) == 0) {
                    
                    uint16_t num_channels = 0;
                    uint32_t sample_rate = 0;
                    uint16_t bit_depth = 0;
                    bool found_data = false;

                    while (!found_data && !feof(f)) {
                        char chunk_id[4];
                        uint32_t chunk_size;
                        if (fread(chunk_id, 1, 4, f) != 4 || fread(&chunk_size, 1, 4, f) != 4) break;

                        if (strncmp(chunk_id, "fmt ", 4) == 0) {
                            uint16_t audio_format;
                            fread(&audio_format, 1, 2, f);
                            fread(&num_channels, 1, 2, f);
                            fread(&sample_rate, 1, 4, f);
                            fseek(f, 6, SEEK_CUR); // Skip byte_rate and block_align
                            fread(&bit_depth, 1, 2, f);
                            if (chunk_size > 16) {
                                fseek(f, chunk_size - 16, SEEK_CUR); // Skip extra bytes
                            }
                        } 
                        else if (strncmp(chunk_id, "data", 4) == 0) {
                            found_data = true;
                            break; 
                        } 
                        else {
                            fseek(f, chunk_size, SEEK_CUR); // Skip LIST, INFO, fact, etc
                        }
                    }

                    if (found_data && num_channels > 0 && sample_rate > 0) {
                        esp_codec_dev_sample_info_t fs = {
                            .bits_per_sample = (uint8_t)bit_depth,
                            .channel = (uint8_t)num_channels,
                            .sample_rate = sample_rate
                        };
                        if (esp_codec_dev_open(spk_codec_dev, &fs) == ESP_OK) {
                            codec_opened = true;
                            esp_codec_dev_set_out_vol(spk_codec_dev, app->_volume);
                            
                            // Update UI with song name
                            if (bsp_display_lock(0)) {
                                size_t slash = filepath.find_last_of("/");
                                std::string short_name = (slash != std::string::npos) ? filepath.substr(slash + 1) : filepath;
                                if (app->_status_label) lv_label_set_text_fmt(app->_status_label, "%s", short_name.c_str());
                                bsp_display_unlock();
                            }
                        }
                    } else {
                        ESP_UTILS_LOGE("Failed to find valid fmt/data chunks in %s", filepath.c_str());
                    }
                } else {
                    ESP_UTILS_LOGE("Not a valid RIFF/WAVE file: %s", filepath.c_str());
                }
            } else {
                if (bsp_display_lock(0)) {
                    app->_is_playing = false;
                    app->update_play_button_text();
                    if (app->_status_label) lv_label_set_text(app->_status_label, "Error: File not found");
                    bsp_display_unlock();
                }
            }
        }
        
        // Playback loop
        if (app->_is_playing && f && codec_opened) {
            size_t read_bytes = fread(buf, 1, CHUNK_SIZE, f);
            if (read_bytes > 0) {
                esp_codec_dev_write(spk_codec_dev, buf, read_bytes);
            } else {
                // EOF reached
                if (bsp_display_lock(0)) {
                    app->_is_playing = false;
                    app->update_play_button_text();
                    if (app->_status_label) lv_label_set_text(app->_status_label, "Finished");
                    bsp_display_unlock();
                }
            }
        } else {
            // Idle or Paused - sleep to avoid CPU spinning
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    // Cleanup when app closes
    if (f) fclose(f);
    if (codec_opened) esp_codec_dev_close(spk_codec_dev);
    if (buf) free(buf);
    
    app->_audio_task_handle = NULL;
    vTaskDelete(NULL);
}
