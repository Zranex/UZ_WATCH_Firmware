#include "app_local_music.hpp"
extern const lv_image_dsc_t icon_local_music;
#include "esp_lib_utils.h"
#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <algorithm>
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"

extern esp_codec_dev_handle_t spk_codec_dev;
LV_FONT_DECLARE(font_cinzel_bold_36);

using namespace esp_brookesia;

static AppLocalMusic* g_local_music_app = nullptr;

extern "C" void app_local_music_play_from_ble(const char* song_name) {
    if (!g_local_music_app) return;
    g_local_music_app->play_song_by_name(song_name);
}

extern "C" void app_local_music_release_audio_for_alarm(void) {
    if (!g_local_music_app) return;
    g_local_music_app->release_audio_for_alarm();
}

extern "C" void app_local_music_reload_playlist(void) {
    if (!g_local_music_app) return;
    g_local_music_app->reload_playlist_external();
}

void AppLocalMusic::release_audio_for_alarm() {
    if (_audio_task_handle == NULL) {
        return;
    }
    _is_playing = false;
    _song_changed = false;
    _release_codec_requested = true;
    int wait_ms = 0;
    while (_codec_currently_open && wait_ms < 250) {
        vTaskDelay(pdMS_TO_TICKS(10));
        wait_ms += 10;
    }
    if (bsp_display_lock(50)) {
        update_play_button_text();
        if (!_is_app_closed && _status_label) {
            lv_label_set_text(_status_label, "Duraklatildi");
        }
        bsp_display_unlock();
    }
}

void AppLocalMusic::reload_playlist_external() {
    if (bsp_display_lock(100)) {
        load_playlist();
        if (!_is_app_closed && !_is_playing && _status_label) {
            if (_playlist.empty()) {
                lv_label_set_text(_status_label, "Sarki yok (AirDrop'tan yukleyin)");
            } else {
                lv_label_set_text_fmt(_status_label, "%d sarki hazir", (int)_playlist.size());
            }
        }
        bsp_display_unlock();
    }
}

void AppLocalMusic::play_song_by_name(const char* name) {
    load_playlist();
    for (size_t i = 0; i < _playlist.size(); i++) {
        if (_playlist[i].find(name) != std::string::npos) {
            _current_song_index = (int)i;
            _song_changed = true;
            _is_playing = true;
            _is_app_closed = false;

            if (_audio_task_handle == NULL) {
                xTaskCreate(audio_task, "local_audio_task", 4096, this, 4, &_audio_task_handle);
            }

            if (bsp_display_lock(100)) {
                update_play_button_text();
                bsp_display_unlock();
            }
            return;
        }
    }
}

AppLocalMusic::AppLocalMusic() : systems::phone::App("Muzik", &icon_local_music, true) {
    g_local_music_app = this;
    _bg_obj = nullptr;
    _btn_back = nullptr;
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
    _release_codec_requested = false;
    _codec_currently_open = false;
    _audio_task_handle = NULL;
    _current_song_index = 0;
    _volume = 65;
}

AppLocalMusic::~AppLocalMusic() {
    _is_app_closed = true;
}

static bool has_wav_ext(const std::string& name) {
    if (name.length() <= 4 || name[0] == '.') return false;
    std::string ext = name.substr(name.length() - 4);
    for (auto &c : ext) c = (char)tolower((unsigned char)c);
    return ext == ".wav";
}

void AppLocalMusic::load_playlist() {
    _playlist.clear();
    const char* dirs[] = {
        BSP_SD_MOUNT_POINT "/musics",
        BSP_SD_MOUNT_POINT "/AirDrop",
        BSP_SD_MOUNT_POINT
    };

    for (size_t d_idx = 0; d_idx < 3; d_idx++) {
        DIR *d = opendir(dirs[d_idx]);
        if (!d) continue;

        struct dirent *dir;
        while ((dir = readdir(d)) != NULL) {
            if (dir->d_type == DT_DIR) continue;
            std::string name = dir->d_name;
            if (!has_wav_ext(name)) continue;

            // Keep voice memos (rec_*.wav in SD root) separate from Music unless placed in /musics
            if (d_idx == 2 && name.rfind("rec_", 0) == 0) continue;

            std::string full_path = std::string(dirs[d_idx]) + "/" + name;
            bool duplicate = false;
            for (const auto& existing : _playlist) {
                size_t sl = existing.find_last_of('/');
                std::string base = (sl != std::string::npos) ? existing.substr(sl + 1) : existing;
                if (base == name) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) {
                _playlist.push_back(full_path);
            }
        }
        closedir(d);
    }

    if (_current_song_index >= (int)_playlist.size()) {
        _current_song_index = 0;
    }
}

bool AppLocalMusic::run() {
    if (_bg_obj != nullptr) return true;
    ESP_UTILS_LOGI("Starting AppLocalMusic");

    _is_app_closed = false;

    _bg_obj = lv_obj_create(lv_scr_act());
    lv_obj_set_size(_bg_obj, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(_bg_obj, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(_bg_obj, 0, 0);
    lv_obj_set_style_radius(_bg_obj, 0, 0);
    lv_obj_clear_flag(_bg_obj, LV_OBJ_FLAG_SCROLLABLE);

    // Back Button (<) at Top-Left
    _btn_back = lv_obj_create(_bg_obj);
    lv_obj_add_flag(_btn_back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(_btn_back, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(_btn_back, 0, 0);
    lv_obj_set_size(_btn_back, 44, 44);
    lv_obj_align(_btn_back, LV_ALIGN_TOP_LEFT, 15, 15);
    lv_obj_set_style_radius(_btn_back, 22, 0);
    lv_obj_set_style_bg_color(_btn_back, lv_color_hex(0x222222), 0);
    lv_obj_set_style_bg_opa(_btn_back, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(_btn_back, lv_color_hex(0x444444), 0);
    lv_obj_set_style_border_width(_btn_back, 1, 0);
    lv_obj_set_style_shadow_width(_btn_back, 0, 0);
    lv_obj_add_event_cb(_btn_back, [](lv_event_t* e) {
        AppLocalMusic* app = (AppLocalMusic*)lv_event_get_user_data(e);
        if (app) {
            lv_async_call([](void* data) {
                AppLocalMusic* a = (AppLocalMusic*)data;
                if (a) a->request_close();
            }, app);
        }
    }, LV_EVENT_CLICKED, this);

    lv_obj_t* lbl_back = lv_label_create(_btn_back);
    lv_obj_set_style_text_font(lbl_back, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(lbl_back, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(lbl_back, LV_SYMBOL_LEFT);
    lv_obj_center(lbl_back);

    // Title
    _title_label = lv_label_create(_bg_obj);
    lv_label_set_text(_title_label, "Muzik Calar");
    lv_obj_align(_title_label, LV_ALIGN_TOP_MID, 15, 22);
    lv_obj_set_style_text_color(_title_label, lv_color_hex(0xFFD700), 0);
    lv_obj_set_style_text_font(_title_label, &lv_font_montserrat_24, 0);

    load_playlist();

    // Status label
    _status_label = lv_label_create(_bg_obj);
    lv_obj_set_width(_status_label, 330);
    lv_label_set_long_mode(_status_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    if (_playlist.empty()) {
        lv_label_set_text(_status_label, "Sarki yok (AirDrop'tan gonderin)");
    } else if (_is_playing && _current_song_index < (int)_playlist.size()) {
        size_t sl = _playlist[_current_song_index].find_last_of('/');
        std::string sn = (sl != std::string::npos) ? _playlist[_current_song_index].substr(sl + 1) : _playlist[_current_song_index];
        lv_label_set_text(_status_label, sn.c_str());
    } else {
        lv_label_set_text_fmt(_status_label, "%d sarki hazir", (int)_playlist.size());
    }
    lv_obj_align(_status_label, LV_ALIGN_TOP_MID, 0, 82);
    lv_obj_set_style_text_color(_status_label, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(_status_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_align(_status_label, LV_TEXT_ALIGN_CENTER, 0);

    // Play/Pause Button
    _btn_play = lv_btn_create(_bg_obj);
    lv_obj_set_size(_btn_play, 100, 100);
    lv_obj_align(_btn_play, LV_ALIGN_CENTER, 0, 25);
    lv_obj_set_style_radius(_btn_play, 50, 0);
    lv_obj_set_style_shadow_width(_btn_play, 0, 0);
    lv_obj_set_style_shadow_spread(_btn_play, 0, 0);
    lv_obj_set_style_shadow_opa(_btn_play, LV_OPA_TRANSP, 0);

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
    lv_obj_set_style_shadow_width(_btn_prev, 0, 0);
    lv_obj_set_style_shadow_spread(_btn_prev, 0, 0);
    lv_obj_set_style_shadow_opa(_btn_prev, LV_OPA_TRANSP, 0);
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
    lv_obj_set_style_shadow_width(_btn_next, 0, 0);
    lv_obj_set_style_shadow_spread(_btn_next, 0, 0);
    lv_obj_set_style_shadow_opa(_btn_next, LV_OPA_TRANSP, 0);
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
    lv_obj_set_style_shadow_width(_btn_vol_down, 0, 0);
    lv_obj_set_style_shadow_spread(_btn_vol_down, 0, 0);
    lv_obj_set_style_shadow_opa(_btn_vol_down, LV_OPA_TRANSP, 0);
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
    lv_obj_set_style_shadow_width(_btn_vol_up, 0, 0);
    lv_obj_set_style_shadow_spread(_btn_vol_up, 0, 0);
    lv_obj_set_style_shadow_opa(_btn_vol_up, LV_OPA_TRANSP, 0);
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
        xTaskCreate(audio_task, "local_audio_task", 4096, this, 4, &_audio_task_handle);
    }
    return true;
}

void AppLocalMusic::update_volume(int change) {
    _volume += change;
    if (_volume > 100) _volume = 100;
    if (_volume < 0) _volume = 0;
    if (_vol_label) lv_label_set_text_fmt(_vol_label, "%d%%", _volume);
    if (spk_codec_dev && _codec_currently_open) {
        esp_codec_dev_set_out_vol(spk_codec_dev, _volume);
    }
}

void AppLocalMusic::play_next() {
    if (_playlist.empty()) load_playlist();
    if (_playlist.empty()) return;
    _current_song_index = (_current_song_index + 1) % (int)_playlist.size();
    _song_changed = true;
    _is_playing = true;
    update_play_button_text();
}

void AppLocalMusic::play_prev() {
    if (_playlist.empty()) load_playlist();
    if (_playlist.empty()) return;
    _current_song_index--;
    if (_current_song_index < 0) _current_song_index = (int)_playlist.size() - 1;
    _song_changed = true;
    _is_playing = true;
    update_play_button_text();
}

bool AppLocalMusic::back() {
    return notifyCoreClosed();
}

bool AppLocalMusic::close() {
    stop_playback();
    _is_app_closed = true;

    int wait_count = 0;
    while (_audio_task_handle != NULL && wait_count < 100) {
        vTaskDelay(pdMS_TO_TICKS(10));
        wait_count++;
    }

    if (_bg_obj != nullptr) {
        lv_obj_delete_async(_bg_obj);
        _bg_obj = nullptr;
    }
    _btn_back = nullptr;
    _title_label = nullptr;
    _status_label = nullptr;
    _btn_play = nullptr;
    _lbl_play = nullptr;
    _btn_next = nullptr;
    _btn_prev = nullptr;
    _btn_vol_up = nullptr;
    _btn_vol_down = nullptr;
    _vol_label = nullptr;
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
    if (_is_app_closed || !_bg_obj || !_lbl_play || !_btn_play) return;

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
        load_playlist();
    }
    if (_playlist.empty()) {
        if (_status_label) lv_label_set_text(_status_label, "Sarki bulunamadi (.wav)");
        return;
    }

    if (!_is_playing) {
        _is_playing = true;
        if (!_codec_currently_open || !_song_changed) {
            // Re-open codec cleanly if it was not opened yet or released for alarm
            if (!_codec_currently_open) {
                _song_changed = true;
            }
        }
    }
    update_play_button_text();
}

void AppLocalMusic::stop_playback() {
    _is_playing = false;
    update_play_button_text();
    if (!_is_app_closed && _status_label) lv_label_set_text(_status_label, "Duraklatildi");
}

void AppLocalMusic::audio_task(void *pvParameter) {
    AppLocalMusic * app = (AppLocalMusic *)pvParameter;
    FILE * f = NULL;
    bool codec_opened = false;

    const int CHUNK_SIZE = 2048;
    uint8_t * buf = (uint8_t *)malloc(CHUNK_SIZE);
    if (!buf) {
        ESP_UTILS_LOGE("Failed to allocate audio chunk buffer!");
        app->_audio_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    bool pa_enabled = false;

    while (!app->_is_app_closed) {
        if (app->_release_codec_requested) {
            app->_release_codec_requested = false;
            if (pa_enabled) {
                bsp_audio_power_amp_enable(false);
                pa_enabled = false;
            }
            if (f) {
                fclose(f);
                f = NULL;
            }
            if (codec_opened && spk_codec_dev) {
                esp_codec_dev_close(spk_codec_dev);
                codec_opened = false;
                app->_codec_currently_open = false;
            }
        }

        if (app->_song_changed) {
            app->_song_changed = false;

            if (pa_enabled) {
                bsp_audio_power_amp_enable(false);
                pa_enabled = false;
            }
            if (f) {
                fclose(f);
                f = NULL;
            }
            if (codec_opened) {
                esp_codec_dev_close(spk_codec_dev);
                codec_opened = false;
                app->_codec_currently_open = false;
            }

            if (app->_playlist.empty()) {
                app->_is_playing = false;
                continue;
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
                            fseek(f, 6, SEEK_CUR);
                            fread(&bit_depth, 1, 2, f);
                            if (chunk_size > 16) {
                                fseek(f, chunk_size - 16, SEEK_CUR);
                            }
                        } else if (strncmp(chunk_id, "data", 4) == 0) {
                            found_data = true;
                            break;
                        } else {
                            // RIFF chunks are word-aligned (pad byte if chunk_size is odd)
                            long skip = (long)chunk_size + ((chunk_size & 1) ? 1 : 0);
                            fseek(f, skip, SEEK_CUR);
                        }
                    }

                    if (found_data && num_channels > 0 && sample_rate > 0 && spk_codec_dev) {
                        esp_codec_dev_sample_info_t fs = {
                            .bits_per_sample = (uint8_t)bit_depth,
                            .channel = (uint8_t)num_channels,
                            .channel_mask = 0,
                            .sample_rate = sample_rate,
                            .mclk_multiple = 256,
                        };
                        if (esp_codec_dev_open(spk_codec_dev, &fs) == ESP_OK) {
                            codec_opened = true;
                            app->_codec_currently_open = true;
                            esp_codec_dev_set_out_vol(spk_codec_dev, app->_volume);

                            if (bsp_display_lock(50)) {
                                if (!app->_is_app_closed && app->_status_label) {
                                    size_t slash = filepath.find_last_of('/');
                                    std::string short_name = (slash != std::string::npos) ? filepath.substr(slash + 1) : filepath;
                                    lv_label_set_text_fmt(app->_status_label, "%s", short_name.c_str());
                                }
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
                if (bsp_display_lock(50)) {
                    app->_is_playing = false;
                    app->update_play_button_text();
                    if (!app->_is_app_closed && app->_status_label) lv_label_set_text(app->_status_label, "Dosya acilamadi");
                    bsp_display_unlock();
                }
            }
        }

        if (app->_is_playing && f && codec_opened && spk_codec_dev) {
            if (!pa_enabled) {
                bsp_audio_power_amp_enable(true);
                pa_enabled = true;
            }
            size_t read_bytes = fread(buf, 1, CHUNK_SIZE, f);
            if (read_bytes > 0) {
                esp_codec_dev_write(spk_codec_dev, buf, read_bytes);
                taskYIELD();
            } else {
                // Auto-advance to next song or stop if single song finished
                if (pa_enabled) {
                    bsp_audio_power_amp_enable(false);
                    pa_enabled = false;
                }
                if (app->_playlist.size() > 1) {
                    app->_current_song_index = (app->_current_song_index + 1) % (int)app->_playlist.size();
                    app->_song_changed = true;
                } else {
                    if (bsp_display_lock(50)) {
                        app->_is_playing = false;
                        app->update_play_button_text();
                        if (!app->_is_app_closed && app->_status_label) lv_label_set_text(app->_status_label, "Bitti");
                        bsp_display_unlock();
                    }
                }
            }
        } else {
            if (pa_enabled) {
                bsp_audio_power_amp_enable(false);
                pa_enabled = false;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    if (f) fclose(f);
    if (codec_opened && spk_codec_dev) esp_codec_dev_close(spk_codec_dev);
    app->_codec_currently_open = false;
    bsp_audio_power_amp_enable(false);
    bsp_audio_standby();
    if (buf) free(buf);

    app->_audio_task_handle = NULL;
    vTaskDelete(NULL);
}
