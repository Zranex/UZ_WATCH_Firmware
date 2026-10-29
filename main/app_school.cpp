#include "app_school.hpp"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "bsp/esp-bsp.h"
#include "esp_timer.h"
#include "ui_notification_banner.hpp"

extern "C" {
#include "rtc_lib.h"
#include "display_manager.h"
#include "esp_codec_dev.h"
void bsp_audio_power_amp_enable(bool enable);
}

extern esp_codec_dev_handle_t spk_codec_dev;
extern const lv_image_dsc_t icon_school;

#define TAG "AppSchool"

AppSchool* AppSchool::_instance = nullptr;

struct SchoolPeriod {
    int start_min; // minutes from 00:00 (e.g. 8*60 = 480)
    int end_min;   // minutes from 00:00
    const char* name;
    bool is_break;
};

static const SchoolPeriod s_periods[] = {
    { 8*60 + 0,  8*60 + 40, "1. Ders", false },
    { 8*60 + 40, 8*60 + 50, "1. Tenefus", true },
    { 8*60 + 50, 9*60 + 30, "2. Ders", false },
    { 9*60 + 30, 9*60 + 40, "2. Tenefus", true },
    { 9*60 + 40, 10*60 + 20, "3. Ders", false },
    { 10*60 + 20, 10*60 + 30, "3. Tenefus", true },
    { 10*60 + 30, 11*60 + 10, "4. Ders", false },
    { 11*60 + 10, 11*60 + 20, "4. Tenefus", true },
    { 11*60 + 20, 12*60 + 0,  "5. Ders", false },
    { 12*60 + 0,  12*60 + 45, "Ogle Arasi", true },
    { 12*60 + 45, 13*60 + 25, "6. Ders", false },
    { 13*60 + 25, 13*60 + 35, "6. Tenefus", true },
    { 13*60 + 35, 14*60 + 15, "7. Ders", false },
    { 14*60 + 15, 14*60 + 25, "7. Tenefus", true },
    { 14*60 + 25, 15*60 + 5,  "8. Ders", false }
};

static const int NUM_PERIODS = sizeof(s_periods) / sizeof(s_periods[0]);

static void play_discreet_bell_chime_task(void* pv) {
    if (spk_codec_dev) {
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = 1,
            .channel_mask = 0,
            .sample_rate = 16000,
            .mclk_multiple = 256,
        };
        bsp_audio_power_amp_enable(true);
        if (esp_codec_dev_open(spk_codec_dev, &fs) == ESP_OK) {
            esp_codec_dev_set_out_vol(spk_codec_dev, 25); // 25% quiet volume
            int16_t pcm[1600]; // 100ms at 16kHz
            for (int i = 0; i < 1600; i++) {
                float t = (float)i / 16000.0f;
                float decay = 1.0f - ((float)i / 1600.0f);
                float s = sinf(2.0f * M_PI * 932.0f * t) * decay * 0.4f; // ~B♭5 warm bell chime
                pcm[i] = (int16_t)(s * 32767.0f);
            }
            esp_codec_dev_write(spk_codec_dev, pcm, sizeof(pcm));
            esp_codec_dev_close(spk_codec_dev);
        }
        bsp_audio_power_amp_enable(false);
    }
    vTaskDelete(NULL);
}

bool AppSchool::is_lesson_in_progress() {
    struct tm timeinfo;
    if (rtc_get_time(&timeinfo) != ESP_OK) return false;
    if (timeinfo.tm_wday < 1 || timeinfo.tm_wday > 5) return false;
    int current_min = timeinfo.tm_hour * 60 + timeinfo.tm_min;
    for (int i = 0; i < NUM_PERIODS; i++) {
        if (!s_periods[i].is_break && current_min >= s_periods[i].start_min && current_min < s_periods[i].end_min) {
            return true;
        }
    }
    return false;
}

static esp_timer_handle_t s_school_esp_timer = NULL;
static void school_timer_esp_cb(void* arg) {
    AppSchool::check_bell_tick();
}

void AppSchool::start_background_timer() {
    if (s_school_esp_timer != NULL) return;
    esp_timer_create_args_t timer_args = {
        .callback = school_timer_esp_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "school_bell_bg",
        .skip_unhandled_events = true
    };
    if (esp_timer_create(&timer_args, &s_school_esp_timer) == ESP_OK) {
        esp_timer_start_periodic(s_school_esp_timer, 1000000); // Ticks every 1 sec independent of LVGL
        ESP_LOGI(TAG, "Hardware RTC-backed school bell timer running (1Hz)");
    }
}

void AppSchool::check_bell_tick() {
    struct tm timeinfo;
    if (rtc_get_time(&timeinfo) != ESP_OK) return;

    // Only active Monday - Friday (1 = Mon, 5 = Fri)
    if (timeinfo.tm_wday < 1 || timeinfo.tm_wday > 5) return;

    // Only trigger at exact second 0
    if (timeinfo.tm_sec != 0) return;

    int current_min = timeinfo.tm_hour * 60 + timeinfo.tm_min;

    for (int i = 0; i < NUM_PERIODS; i++) {
        if (current_min == s_periods[i].start_min) {
            // New period started!
            char banner[64];
            if (s_periods[i].is_break) {
                snprintf(banner, sizeof(banner), "Zil Caldi! %s Basladi", s_periods[i].name);
            } else {
                snprintf(banner, sizeof(banner), "Ders Zili Caldi: %s", s_periods[i].name);
            }
            display_manager_turn_on();
            if (bsp_display_lock(1000)) {
                AppNotifications::show("Okul Asistani", banner);
                bsp_display_unlock();
            }
            xTaskCreate(play_discreet_bell_chime_task, "bell_sound", 3072, NULL, 4, NULL);
            return;
        }
    }

    // Check school dismissal (15:05)
    if (current_min == (15*60 + 5)) {
        display_manager_turn_on();
        if (bsp_display_lock(1000)) {
            AppNotifications::show("Okul Asistani", "Okul Bitti! Iyi Tatiller!");
            bsp_display_unlock();
        }
        xTaskCreate(play_discreet_bell_chime_task, "bell_sound", 3072, NULL, 4, NULL);
    }
}

static void on_school_back_clicked(lv_event_t* e) {
    AppSchool* app = (AppSchool*)lv_event_get_user_data(e);
    if (app) {
        lv_async_call([](void* data) {
            AppSchool* a = (AppSchool*)data;
            if (a) {
                a->request_close();
            }
        }, app);
    }
}

AppSchool::AppSchool()
    : esp_brookesia::systems::phone::App("Okul", &icon_school, true),
      _bg_obj(nullptr), _btn_back(nullptr), _arc_progress(nullptr),
      _lbl_status(nullptr), _lbl_time_left(nullptr), _lbl_next(nullptr),
      _list_schedule(nullptr), _poll_timer(nullptr)
{
    _instance = this;
}

AppSchool::~AppSchool() {
    if (_instance == this) _instance = nullptr;
}

bool AppSchool::init() {
    _instance = this;
    return true;
}

bool AppSchool::run() {
    if (_bg_obj != nullptr) return true;
    _instance = this;

    lv_obj_t* scr = lv_scr_act();
    _bg_obj = lv_obj_create(scr);
    lv_obj_set_size(_bg_obj, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(_bg_obj, lv_color_hex(0x0A0E1A), 0);
    lv_obj_set_style_bg_opa(_bg_obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_bg_obj, 0, 0);
    lv_obj_set_style_radius(_bg_obj, 0, 0);
    lv_obj_set_style_shadow_width(_bg_obj, 0, 0);
    lv_obj_set_style_shadow_spread(_bg_obj, 0, 0);
    lv_obj_set_style_shadow_opa(_bg_obj, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(_bg_obj, LV_OBJ_FLAG_SCROLLABLE);

    // Top Header: Back Button (<)
    _btn_back = lv_obj_create(_bg_obj);
    lv_obj_add_flag(_btn_back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(_btn_back, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(_btn_back, 0, 0);
    lv_obj_set_size(_btn_back, 44, 44);
    lv_obj_align(_btn_back, LV_ALIGN_TOP_LEFT, 15, 15);
    lv_obj_set_style_radius(_btn_back, 22, 0);
    lv_obj_set_style_bg_color(_btn_back, lv_color_hex(0x1A233A), 0);
    lv_obj_set_style_bg_opa(_btn_back, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(_btn_back, lv_color_hex(0x2E3D66), 0);
    lv_obj_set_style_border_width(_btn_back, 1, 0);
    lv_obj_set_style_shadow_width(_btn_back, 0, 0);
    lv_obj_set_style_shadow_spread(_btn_back, 0, 0);
    lv_obj_set_style_shadow_opa(_btn_back, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(_btn_back, on_school_back_clicked, LV_EVENT_CLICKED, this);

    lv_obj_t* lbl_back = lv_label_create(_btn_back);
    lv_obj_set_style_text_font(lbl_back, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(lbl_back, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(lbl_back, LV_SYMBOL_LEFT);
    lv_obj_center(lbl_back);

    // Title
    lv_obj_t* lbl_title = lv_label_create(_bg_obj);
    lv_obj_set_style_text_color(lbl_title, lv_color_hex(0xFFC107), 0);
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_20, 0);
    lv_label_set_text(lbl_title, "Okul Asistani");
    lv_obj_align(lbl_title, LV_ALIGN_TOP_LEFT, 68, 24);

    // Circular Countdown Arc
    _arc_progress = lv_arc_create(_bg_obj);
    lv_obj_set_size(_arc_progress, 190, 190);
    lv_obj_align(_arc_progress, LV_ALIGN_TOP_MID, 0, 75);
    lv_arc_set_rotation(_arc_progress, 135);
    lv_arc_set_bg_angles(_arc_progress, 0, 270);
    lv_arc_set_range(_arc_progress, 0, 100);
    lv_arc_set_value(_arc_progress, 0);
    lv_obj_remove_style(_arc_progress, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(_arc_progress, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(_arc_progress, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_color(_arc_progress, lv_color_hex(0x1F2A44), LV_PART_MAIN);
    lv_obj_set_style_arc_width(_arc_progress, 10, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(_arc_progress, lv_color_hex(0xFFC107), LV_PART_INDICATOR);
    lv_obj_set_style_shadow_width(_arc_progress, 0, 0);
    lv_obj_set_style_shadow_spread(_arc_progress, 0, 0);
    lv_obj_set_style_shadow_opa(_arc_progress, LV_OPA_TRANSP, 0);

    // Status Label inside Arc (e.g. "3. DERS")
    _lbl_status = lv_label_create(_arc_progress);
    lv_obj_set_style_text_color(_lbl_status, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(_lbl_status, &lv_font_montserrat_20, 0);
    lv_label_set_text(_lbl_status, "Yukleniyor...");
    lv_obj_align(_lbl_status, LV_ALIGN_CENTER, 0, -22);

    // Time Left Label inside Arc (e.g. "18 dk kaldi")
    _lbl_time_left = lv_label_create(_arc_progress);
    lv_obj_set_style_text_color(_lbl_time_left, lv_color_hex(0xFFC107), 0);
    lv_obj_set_style_text_font(_lbl_time_left, &lv_font_montserrat_16, 0);
    lv_label_set_text(_lbl_time_left, "");
    lv_obj_align(_lbl_time_left, LV_ALIGN_CENTER, 0, 6);

    // Next Period Subtitle inside Arc
    _lbl_next = lv_label_create(_arc_progress);
    lv_obj_set_style_text_color(_lbl_next, lv_color_hex(0x8A9BB8), 0);
    lv_obj_set_style_text_font(_lbl_next, &lv_font_montserrat_12, 0);
    lv_label_set_text(_lbl_next, "");
    lv_obj_align(_lbl_next, LV_ALIGN_CENTER, 0, 30);

    // Scrollable Schedule List Container (Bottom Half)
    _list_schedule = lv_obj_create(_bg_obj);
    lv_obj_set_size(_list_schedule, 370, 205);
    lv_obj_align(_list_schedule, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(_list_schedule, lv_color_hex(0x121826), 0);
    lv_obj_set_style_bg_opa(_list_schedule, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(_list_schedule, lv_color_hex(0x222C42), 0);
    lv_obj_set_style_border_width(_list_schedule, 1, 0);
    lv_obj_set_style_radius(_list_schedule, 14, 0);
    lv_obj_set_style_pad_all(_list_schedule, 8, 0);
    lv_obj_set_style_shadow_width(_list_schedule, 0, 0);
    lv_obj_set_style_shadow_spread(_list_schedule, 0, 0);
    lv_obj_set_style_shadow_opa(_list_schedule, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(_list_schedule, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(_list_schedule, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    update_ui();

    // 1-second refresh timer
    _poll_timer = lv_timer_create([](lv_timer_t* t) {
        AppSchool* app = (AppSchool*)t->user_data;
        if (app) app->update_ui();
    }, 1000, this);

    return true;
}

void AppSchool::update_ui() {
    if (!_lbl_status || !_arc_progress) return;

    struct tm timeinfo;
    if (rtc_get_time(&timeinfo) != ESP_OK) return;

    bool is_weekend = (timeinfo.tm_wday == 0 || timeinfo.tm_wday == 6);
    int current_sec_of_day = (timeinfo.tm_hour * 3600) + (timeinfo.tm_min * 60) + timeinfo.tm_sec;
    int current_min = current_sec_of_day / 60;

    if (is_weekend) {
        lv_label_set_text(_lbl_status, "Hafta Sonu!");
        lv_label_set_text(_lbl_time_left, "Iyi Dinlenmeler");
        lv_label_set_text(_lbl_next, "Pazartesi 08:00");
        lv_arc_set_value(_arc_progress, 100);
        lv_obj_set_style_arc_color(_arc_progress, lv_color_hex(0x00E676), LV_PART_INDICATOR);
        return;
    }

    int active_idx = -1;
    for (int i = 0; i < NUM_PERIODS; i++) {
        if (current_min >= s_periods[i].start_min && current_min < s_periods[i].end_min) {
            active_idx = i;
            break;
        }
    }

    if (active_idx != -1) {
        const SchoolPeriod& p = s_periods[active_idx];
        lv_label_set_text(_lbl_status, p.name);

        int period_dur_sec = (p.end_min - p.start_min) * 60;
        int elapsed_sec = current_sec_of_day - (p.start_min * 60);
        int remaining_sec = period_dur_sec - elapsed_sec;
        int rem_min = (remaining_sec + 59) / 60;

        char buf[32];
        snprintf(buf, sizeof(buf), "%d dk kaldi", rem_min);
        lv_label_set_text(_lbl_time_left, buf);

        if (active_idx + 1 < NUM_PERIODS) {
            char next_buf[48];
            snprintf(next_buf, sizeof(next_buf), "Sirada: %s", s_periods[active_idx + 1].name);
            lv_label_set_text(_lbl_next, next_buf);
        } else {
            lv_label_set_text(_lbl_next, "Sonra: Okul Bitisi!");
        }

        int pct = (elapsed_sec * 100) / period_dur_sec;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        lv_arc_set_value(_arc_progress, pct);

        if (p.is_break) {
            lv_obj_set_style_arc_color(_arc_progress, lv_color_hex(0x00E676), LV_PART_INDICATOR); // Green for break
        } else {
            lv_obj_set_style_arc_color(_arc_progress, lv_color_hex(0xFFC107), LV_PART_INDICATOR); // Gold for lesson
        }
    } else if (current_min < s_periods[0].start_min) {
        lv_label_set_text(_lbl_status, "Okul Baslamadi");
        int rem_min = s_periods[0].start_min - current_min;
        char buf[32];
        snprintf(buf, sizeof(buf), "%d dk sonra zil", rem_min);
        lv_label_set_text(_lbl_time_left, buf);
        lv_label_set_text(_lbl_next, "1. Ders: 08:00");
        lv_arc_set_value(_arc_progress, 0);
    } else {
        lv_label_set_text(_lbl_status, "Okul Bitti!");
        lv_label_set_text(_lbl_time_left, "Iyi Tatiller");
        lv_label_set_text(_lbl_next, "Yarin 08:00");
        lv_arc_set_value(_arc_progress, 100);
        lv_obj_set_style_arc_color(_arc_progress, lv_color_hex(0x2196F3), LV_PART_INDICATOR);
    }

    // Refresh list entries if empty
    if (_list_schedule && lv_obj_get_child_cnt(_list_schedule) == 0) {
        for (int i = 0; i < NUM_PERIODS; i++) {
            const SchoolPeriod& sp = s_periods[i];
            lv_obj_t* row = lv_obj_create(_list_schedule);
            lv_obj_set_size(row, LV_PCT(100), 40);
            lv_obj_set_style_bg_color(row, sp.is_break ? lv_color_hex(0x182033) : lv_color_hex(0x141A29), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(row, 8, 0);
            lv_obj_set_style_border_width(row, 0, 0);
            lv_obj_set_style_pad_all(row, 6, 0);
            lv_obj_set_style_shadow_width(row, 0, 0);
            lv_obj_set_style_shadow_spread(row, 0, 0);
            lv_obj_set_style_shadow_opa(row, LV_OPA_TRANSP, 0);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

            int sh = sp.start_min / 60;
            int sm = sp.start_min % 60;
            int eh = sp.end_min / 60;
            int em = sp.end_min % 60;

            char tbuf[32];
            snprintf(tbuf, sizeof(tbuf), "%02d:%02d - %02d:%02d", sh, sm, eh, em);

            lv_obj_t* lbl_t = lv_label_create(row);
            lv_obj_set_style_text_font(lbl_t, &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_color(lbl_t, lv_color_hex(0x8A9BB8), 0);
            lv_label_set_text(lbl_t, tbuf);
            lv_obj_align(lbl_t, LV_ALIGN_LEFT_MID, 6, 0);

            lv_obj_t* lbl_n = lv_label_create(row);
            lv_obj_set_style_text_font(lbl_n, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(lbl_n, sp.is_break ? lv_color_hex(0x00E676) : lv_color_hex(0xFFC107), 0);
            lv_label_set_text(lbl_n, sp.name);
            lv_obj_align(lbl_n, LV_ALIGN_RIGHT_MID, -6, 0);
        }
    }
}

bool AppSchool::back() {
    return notifyCoreClosed();
}

bool AppSchool::close() {
    if (_poll_timer) {
        lv_timer_del(_poll_timer);
        _poll_timer = nullptr;
    }

    if (_bg_obj != nullptr) {
        lv_obj_delete_async(_bg_obj);
        _bg_obj = nullptr;
    }
    _btn_back = nullptr;
    _arc_progress = nullptr;
    _lbl_status = nullptr;
    _lbl_time_left = nullptr;
    _lbl_next = nullptr;
    _list_schedule = nullptr;
    return true;
}
