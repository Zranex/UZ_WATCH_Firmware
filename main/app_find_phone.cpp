#include "app_find_phone.hpp"
extern const lv_image_dsc_t icon_find_phone;
#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "bsp_board_extra.h"
#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "display_manager.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "app_local_music.hpp"
#include "app_tars.hpp"
#include <math.h>

extern "C" {
#include "ble_manager.h"
}

#define TAG "FindPhone"

extern esp_codec_dev_handle_t spk_codec_dev;

static AppFindPhone* s_find_phone_instance = nullptr;

static TaskHandle_t s_find_watch_task = NULL;
static volatile bool s_find_watch_ringing = false;
static lv_obj_t* s_find_watch_modal = NULL;

extern "C" void app_find_phone_handle_stop_ack(void) {
    if (s_find_phone_instance) {
        if (display_manager_is_on() && bsp_display_lock(1000)) {
            s_find_phone_instance->reset_state();
            bsp_display_unlock();
        } else {
            s_find_phone_instance->reset_state_offline();
        }
    }
}

static void create_find_watch_modal_ui(void) {
    if (!bsp_display_lock(500)) return;

    if (s_find_watch_modal != NULL) {
        lv_obj_del(s_find_watch_modal);
        s_find_watch_modal = NULL;
    }

    s_find_watch_modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_find_watch_modal, 360, 320);
    lv_obj_center(s_find_watch_modal);
    lv_obj_set_style_radius(s_find_watch_modal, 24, 0);
    lv_obj_set_style_bg_color(s_find_watch_modal, lv_color_hex(0x1A0505), 0);
    lv_obj_set_style_border_color(s_find_watch_modal, lv_color_hex(0xFF3D00), 0);
    lv_obj_set_style_border_width(s_find_watch_modal, 3, 0);
    lv_obj_set_style_shadow_width(s_find_watch_modal, 0, 0);
    lv_obj_clear_flag(s_find_watch_modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* icon_lbl = lv_label_create(s_find_watch_modal);
    lv_obj_set_style_text_font(icon_lbl, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(icon_lbl, lv_color_hex(0xFF5252), 0);
    lv_label_set_text(icon_lbl, LV_SYMBOL_BELL);
    lv_obj_align(icon_lbl, LV_ALIGN_TOP_MID, 0, 15);

    lv_obj_t* t_lbl = lv_label_create(s_find_watch_modal);
    lv_obj_set_style_text_font(t_lbl, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(t_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(t_lbl, "TELEFON ARIYOR!");
    lv_obj_align(t_lbl, LV_ALIGN_TOP_MID, 0, 68);

    lv_obj_t* s_lbl = lv_label_create(s_find_watch_modal);
    lv_obj_set_style_text_font(s_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_lbl, lv_color_hex(0xCCCCCC), 0);
    lv_label_set_text(s_lbl, "Saat alarm caliyor...");
    lv_obj_align(s_lbl, LV_ALIGN_TOP_MID, 0, 105);

    lv_obj_t* btn_stop = lv_btn_create(s_find_watch_modal);
    lv_obj_set_size(btn_stop, 240, 64);
    lv_obj_align(btn_stop, LV_ALIGN_BOTTOM_MID, 0, -25);
    lv_obj_set_style_radius(btn_stop, 32, 0);
    lv_obj_set_style_bg_color(btn_stop, lv_color_hex(0x00E676), 0);
    lv_obj_set_style_shadow_width(btn_stop, 0, 0);
    lv_obj_set_style_shadow_spread(btn_stop, 0, 0);
    lv_obj_set_style_shadow_opa(btn_stop, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(btn_stop, [](lv_event_t* e) {
        app_find_watch_stop_alarm();
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t* stop_lbl = lv_label_create(btn_stop);
    lv_obj_set_style_text_font(stop_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(stop_lbl, lv_color_hex(0x000000), 0);
    lv_label_set_text(stop_lbl, LV_SYMBOL_MUTE "  DURDUR");
    lv_obj_center(stop_lbl);

    bsp_display_unlock();
}

static void find_watch_alarm_task(void* pv) {
    ESP_LOGI(TAG, "Watch Finder Alarm task started (Loud Siren + Motor)");

    // Wake display & create modal from this 8KB worker task (never from 4KB nimble_host stack!)
    display_manager_turn_on();
    create_find_watch_modal_ui();

    // Cleanly yield TARS & local music from shared codec before starting siren
    app_tars_stop_all_audio();
    app_local_music_release_audio_for_alarm();
    vTaskDelay(pdMS_TO_TICKS(60));

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 0,
        .sample_rate = 16000,
        .mclk_multiple = 256,
    };

    bool audio_locked = bsp_audio_lock(1000);
    bool codec_ok = false;
    if (audio_locked && spk_codec_dev) {
        bsp_audio_power_amp_enable(true);
        if (esp_codec_dev_open(spk_codec_dev, &fs) == ESP_OK) {
            codec_ok = true;
            esp_codec_dev_set_out_vol(spk_codec_dev, 90); // 90% loud & brownout-safe
        } else {
            ESP_LOGE(TAG, "Failed to open speaker codec for alarm");
        }
    } else {
        ESP_LOGE(TAG, "Audio bus busy for alarm");
    }

    const int SAMPLES_PER_TONE = 1600; // 100ms at 16kHz
    int16_t* pcm_low = (int16_t*)malloc(SAMPLES_PER_TONE * sizeof(int16_t));
    int16_t* pcm_high = (int16_t*)malloc(SAMPLES_PER_TONE * sizeof(int16_t));

    if (pcm_low && pcm_high) {
        for (int i = 0; i < SAMPLES_PER_TONE; i++) {
            float t = (float)i / 16000.0f;
            float s1 = sinf(2.0f * (float)M_PI * 1046.0f * t); // C6
            float s2 = sinf(2.0f * (float)M_PI * 1568.0f * t); // G6
            pcm_low[i] = (int16_t)(s1 * 28000.0f);
            pcm_high[i] = (int16_t)(s2 * 28000.0f);
        }
    }

    int cycles = 0;
    while (s_find_watch_ringing && cycles < 30) {
        display_manager_reset_timer(); // Keep screen awake during entire alarm!
        // Stagger motor haptic BEFORE siren writes so motor + 5V speaker PA never peak together!
        if ((cycles % 2) == 0) {
            bsp_vibrate_pulse(100);
            vTaskDelay(pdMS_TO_TICKS(115));
            if (!s_find_watch_ringing) break;
        }

        if (codec_ok && pcm_low) {
            esp_codec_dev_write(spk_codec_dev, pcm_low, SAMPLES_PER_TONE * sizeof(int16_t));
        }
        vTaskDelay(pdMS_TO_TICKS(15));
        if (!s_find_watch_ringing) break;

        if (codec_ok && pcm_high) {
            esp_codec_dev_write(spk_codec_dev, pcm_high, SAMPLES_PER_TONE * sizeof(int16_t));
        }
        vTaskDelay(pdMS_TO_TICKS(15));
        if (!s_find_watch_ringing) break;

        cycles++;
    }

    if (pcm_low) free(pcm_low);
    if (pcm_high) free(pcm_high);

    if (audio_locked) {
        if (codec_ok && spk_codec_dev) {
            esp_codec_dev_set_out_vol(spk_codec_dev, 70);
            esp_codec_dev_close(spk_codec_dev);
        }
        bsp_audio_power_amp_enable(false);
        bsp_audio_standby();
        bsp_audio_unlock();
    }

    s_find_watch_ringing = false;

    if (bsp_display_lock(300)) {
        if (s_find_watch_modal != NULL) {
            lv_obj_delete_async(s_find_watch_modal);
            s_find_watch_modal = NULL;
        }
        bsp_display_unlock();
    }

    ble_manager_send_media_command("FIND_WATCH_STOPPED");

    s_find_watch_task = NULL;
    vTaskDelete(NULL);
}

extern "C" void app_find_watch_stop_alarm(void) {
    ESP_LOGI(TAG, "Stopping Find Watch Alarm...");
    s_find_watch_ringing = false;
    bsp_vibrate_pulse(60);
}

extern "C" void app_find_watch_start_alarm(void) {
    ESP_LOGI(TAG, "Starting Find Watch Alarm (Display + Siren + Top Modal)...");
    if (s_find_watch_ringing || s_find_watch_task != NULL) {
        return;
    }
    s_find_watch_ringing = true;
    xTaskCreate(find_watch_alarm_task, "find_watch_alarm", 8192, NULL, 5, &s_find_watch_task);
}

AppFindPhone* AppFindPhone::get_instance() {
    return s_find_phone_instance;
}

AppFindPhone::AppFindPhone() 
    : esp_brookesia::systems::phone::App("Tel Bul", &icon_find_phone, true),
      _bg_obj(nullptr), _btn_back(nullptr), _btn_alarm(nullptr), _lbl_alarm(nullptr), 
      _status_label(nullptr), _poll_timer(nullptr), _is_alarming(false), _current_state(-1) {
    s_find_phone_instance = this;
}

AppFindPhone::~AppFindPhone() {
    if (s_find_phone_instance == this) {
        s_find_phone_instance = nullptr;
    }
}

void AppFindPhone::on_back_clicked(lv_event_t *e) {
    AppFindPhone *self = (AppFindPhone *)lv_event_get_user_data(e);
    if (self) {
        lv_async_call([](void *data) {
            AppFindPhone *app = (AppFindPhone *)data;
            if (app) {
                app->request_close();
            }
        }, self);
    }
}

bool AppFindPhone::run() {
    if (_bg_obj != nullptr) return true;
    s_find_phone_instance = this;

    lv_obj_t* scr = lv_scr_act();
    _bg_obj = lv_obj_create(scr);
    lv_obj_set_size(_bg_obj, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(_bg_obj, lv_color_hex(0x121212), 0);
    lv_obj_set_style_border_width(_bg_obj, 0, 0);
    lv_obj_set_style_radius(_bg_obj, 0, 0);
    lv_obj_set_style_shadow_width(_bg_obj, 0, 0);
    lv_obj_set_style_shadow_spread(_bg_obj, 0, 0);
    lv_obj_set_style_shadow_opa(_bg_obj, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(_bg_obj, LV_OBJ_FLAG_SCROLLABLE);

    // 1. Back Button (<) at Top-Left
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
    lv_obj_set_style_shadow_spread(_btn_back, 0, 0);
    lv_obj_set_style_shadow_opa(_btn_back, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(_btn_back, on_back_clicked, LV_EVENT_CLICKED, this);

    lv_obj_t* lbl_back = lv_label_create(_btn_back);
    lv_obj_set_style_text_font(lbl_back, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(lbl_back, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(lbl_back, LV_SYMBOL_LEFT);
    lv_obj_align(lbl_back, LV_ALIGN_CENTER, 0, 0);

    // 2. Title
    lv_obj_t* title_label = lv_label_create(_bg_obj);
    lv_label_set_text(title_label, "Telefonumu Bul");
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title_label, LV_ALIGN_TOP_LEFT, 70, 24);

    // 3. Status Label
    _status_label = lv_label_create(_bg_obj);
    lv_obj_set_style_text_font(_status_label, &lv_font_montserrat_14, 0);
    lv_obj_align(_status_label, LV_ALIGN_CENTER, 0, -110);

    // 4. Big Circular Alarm Button
    _btn_alarm = lv_obj_create(_bg_obj);
    lv_obj_add_flag(_btn_alarm, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(_btn_alarm, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(_btn_alarm, 0, 0);
    lv_obj_set_size(_btn_alarm, 170, 170);
    lv_obj_align(_btn_alarm, LV_ALIGN_CENTER, 0, 20);
    lv_obj_set_style_radius(_btn_alarm, 85, 0);
    lv_obj_set_style_border_width(_btn_alarm, 0, 0);
    lv_obj_set_style_shadow_width(_btn_alarm, 0, 0);
    lv_obj_set_style_shadow_spread(_btn_alarm, 0, 0);
    lv_obj_set_style_shadow_opa(_btn_alarm, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(_btn_alarm, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(_btn_alarm, btn_alarm_cb, LV_EVENT_CLICKED, this);

    _lbl_alarm = lv_label_create(_btn_alarm);
    lv_obj_set_style_text_font(_lbl_alarm, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_align(_lbl_alarm, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(_lbl_alarm, LV_ALIGN_CENTER, 0, 0);

    // 5. Periodic status refresh timer (checks BLE connection)
    _poll_timer = lv_timer_create([](lv_timer_t* t) {
        AppFindPhone* app = (AppFindPhone*)t->user_data;
        if (app) {
            app->update_ui();
        }
    }, 1000, this);

    _current_state = -1;
    update_ui();
    return true;
}

bool AppFindPhone::back() {
    return notifyCoreClosed();
}

bool AppFindPhone::close() {
    if (_poll_timer) {
        lv_timer_del(_poll_timer);
        _poll_timer = nullptr;
    }

    if (_is_alarming) {
        _is_alarming = false;
        ble_manager_send_media_command("FIND_PHONE_STOP");
    }

    if (_bg_obj != nullptr) {
        lv_obj_delete_async(_bg_obj);
        _bg_obj = nullptr;
    }
    _btn_back = nullptr;
    _btn_alarm = nullptr;
    _lbl_alarm = nullptr;
    _status_label = nullptr;
    _current_state = -1;
    return true;
}

void AppFindPhone::reset_state() {
    _is_alarming = false;
    update_ui();
}

void AppFindPhone::reset_state_offline() {
    _is_alarming = false;
    _current_state = -1;
}

void AppFindPhone::update_ui() {
    if (!_status_label || !_btn_alarm || !_lbl_alarm) return;

    bool connected = ble_manager_is_connected();
    int target_state = 0;

    if (!connected) {
        target_state = 0;
        _is_alarming = false;
    } else if (_is_alarming) {
        target_state = 2;
    } else {
        target_state = 1;
    }

    if (target_state == _current_state) {
        return;
    }
    _current_state = target_state;

    if (target_state == 0) {
        lv_label_set_text(_status_label, "Bluetooth Bagli Degil!");
        lv_obj_set_style_text_color(_status_label, lv_color_hex(0xFFA500), 0);
        lv_obj_set_style_bg_color(_btn_alarm, lv_color_hex(0x333333), 0);
        lv_label_set_text(_lbl_alarm, LV_SYMBOL_WARNING "\nBAGLANTI\nYOK");
    } else if (target_state == 2) {
        lv_label_set_text(_status_label, "Telefon Caliniyor...");
        lv_obj_set_style_text_color(_status_label, lv_color_hex(0xFF5252), 0);
        lv_obj_set_style_bg_color(_btn_alarm, lv_color_hex(0xD32F2F), 0);
        lv_label_set_text(_lbl_alarm, LV_SYMBOL_MUTE "\nDURDUR");
    } else {
        lv_label_set_text(_status_label, "Bagli - Calmaya Hazir");
        lv_obj_set_style_text_color(_status_label, lv_color_hex(0x00E676), 0);
        lv_obj_set_style_bg_color(_btn_alarm, lv_color_hex(0x1976D2), 0);
        lv_label_set_text(_lbl_alarm, LV_SYMBOL_BELL "\nALARM\nCAL");
    }
}

void AppFindPhone::btn_alarm_cb(lv_event_t *e) {
    AppFindPhone *self = (AppFindPhone *)lv_event_get_user_data(e);
    if (!self) return;

    if (!ble_manager_is_connected()) {
        ESP_LOGW(TAG, "Cannot ring phone: BLE not connected");
        if (self->_status_label) {
            lv_label_set_text(self->_status_label, "Once Telefona Baglanin!");
            lv_obj_set_style_text_color(self->_status_label, lv_color_hex(0xFF5252), 0);
        }
        return;
    }

    if (self->_is_alarming) {
        self->_is_alarming = false;
        ESP_LOGI(TAG, "Stopping Find Phone alarm...");
        ble_manager_send_media_command("FIND_PHONE_STOP");
    } else {
        self->_is_alarming = true;
        ESP_LOGI(TAG, "Starting Find Phone alarm...");
        ble_manager_send_media_command("FIND_PHONE_START");
    }
    self->update_ui();
}
