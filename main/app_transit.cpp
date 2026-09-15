#include "app_transit.hpp"
#include "esp_lib_utils.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

extern "C" {
#include "ble_manager.h"
#include "wifi_manager.h"
#include "rtc_lib.h"
#include "display_manager.h"
}

// C Wrapper for LVGL display lock
extern "C" bool bsp_display_lock(uint32_t timeout_ms);
extern "C" void bsp_display_unlock(void);

extern const lv_image_dsc_t icon_transit;

#define TAG "AppTransit"

AppTransit* AppTransit::_instance = nullptr;

AppTransit::AppTransit() 
    : esp_brookesia::systems::phone::App("Ulasim", &icon_transit, true),
      _bg_obj(nullptr), _lbl_title(nullptr), _lbl_status(nullptr),
      _btn_refresh(nullptr), _lbl_refresh(nullptr), _is_fetching(false)
{
    _instance = this;

    // Route 0: 55T Gidis (Fener -> Harbiye / Taksim)
    _routes[0].badge_name = "55T";
    _routes[0].badge_color = lv_color_hex(0xFFA000); // Amber / Yellow
    _routes[0].route_name = "Fener -> Harbiye";
    _routes[0].direction = "Gidis: Taksim Yonu";
    _routes[0].current_time = "Bekleniyor";
    _routes[0].current_sub = "Dokun: Yenile";
    _routes[0].card_obj = nullptr;
    _routes[0].lbl_time = nullptr;
    _routes[0].lbl_sub = nullptr;

    // Route 1: M2 Metro Donus (Osmanbey -> Halic)
    _routes[1].badge_name = "M2";
    _routes[1].badge_color = lv_color_hex(0x009E49); // Metro Istanbul Green
    _routes[1].route_name = "Osmanbey -> Halic";
    _routes[1].direction = "Donus: Yenikapi Yonu";
    _routes[1].current_time = "Bekleniyor";
    _routes[1].current_sub = "Her 3-5 dk bir";
    _routes[1].card_obj = nullptr;
    _routes[1].lbl_time = nullptr;
    _routes[1].lbl_sub = nullptr;

    // Route 2: T5 Tramvay Donus (Kucukpazar -> Fener)
    _routes[2].badge_name = "T5";
    _routes[2].badge_color = lv_color_hex(0x7E317D); // Tramvay Purple
    _routes[2].route_name = "K.Pazar -> Fener";
    _routes[2].direction = "Donus: Alibeykoy Yonu";
    _routes[2].current_time = "Bekleniyor";
    _routes[2].current_sub = "Her 5-7 dk bir";
    _routes[2].card_obj = nullptr;
    _routes[2].lbl_time = nullptr;
    _routes[2].lbl_sub = nullptr;

    // Route 3: 55T Alternatif Donus (Harbiye Muze -> Fener)
    _routes[3].badge_name = "55T";
    _routes[3].badge_color = lv_color_hex(0x1976D2); // Blue
    _routes[3].route_name = "Harbiye -> Fener";
    _routes[3].direction = "Donus: GOP Yonu";
    _routes[3].current_time = "Bekleniyor";
    _routes[3].current_sub = "Alternatif Hat";
    _routes[3].card_obj = nullptr;
    _routes[3].lbl_time = nullptr;
    _routes[3].lbl_sub = nullptr;

    compute_offline_estimates();
}

AppTransit::~AppTransit() {
    if (_instance == this) _instance = nullptr;
}

bool AppTransit::init() {
    _instance = this;
    return true;
}

void AppTransit::compute_offline_estimates() {
    struct tm timeinfo;
    if (rtc_get_time(&timeinfo) == ESP_OK) {
        int hour = timeinfo.tm_hour;
        int min = timeinfo.tm_min;

        if (hour >= 6 && hour <= 23) {
            // M2 Metro (Runs every 4 mins)
            int m2_wait = 4 - (min % 4);
            char buf[32];
            snprintf(buf, sizeof(buf), "%d dk", m2_wait == 0 ? 4 : m2_wait);
            _routes[1].current_time = buf;
            _routes[1].current_sub = "Tarife: ~4 dk (Cevrimdisi)";

            // T5 Tramvay (Runs every 6 mins)
            int t5_wait = 6 - (min % 6);
            snprintf(buf, sizeof(buf), "%d dk", t5_wait == 0 ? 6 : t5_wait);
            _routes[2].current_time = buf;
            _routes[2].current_sub = "Tarife: ~6 dk (Cevrimdisi)";

            // 55T Fener (Runs every ~12 mins)
            int b55_wait = 12 - (min % 12);
            snprintf(buf, sizeof(buf), "%d dk", b55_wait == 0 ? 12 : b55_wait);
            _routes[0].current_time = buf;
            _routes[0].current_sub = "Tarife: ~12 dk (Cevrimdisi)";

            // 55T Harbiye (Runs every ~14 mins)
            int h55_wait = 14 - ((min + 5) % 14);
            snprintf(buf, sizeof(buf), "%d dk", h55_wait == 0 ? 14 : h55_wait);
            _routes[3].current_time = buf;
            _routes[3].current_sub = "Tarife: ~14 dk (Cevrimdisi)";
        } else {
            for (int i = 0; i < 4; i++) {
                _routes[i].current_time = "Sefer Yok";
                _routes[i].current_sub = "Gece Tarifesi (00-06)";
            }
        }
    }
}

void AppTransit::on_refresh_clicked(lv_event_t* e) {
    AppTransit* app = (AppTransit*)lv_event_get_user_data(e);
    if (app) {
        app->request_refresh();
    }
}

void AppTransit::request_refresh() {
    ESP_LOGI(TAG, "Requesting transit live refresh...");
    
    // 1. Instant offline recalculation to provide instant feedback
    compute_offline_estimates();
    if (_lbl_status) {
        lv_label_set_text(_lbl_status, "Canli sorgulaniyor...");
        lv_obj_set_style_text_color(_lbl_status, lv_color_hex(0xFFA000), 0);
    }
    
    for (int i = 0; i < 4; i++) {
        if (_routes[i].lbl_time) lv_label_set_text(_routes[i].lbl_time, _routes[i].current_time.c_str());
        if (_routes[i].lbl_sub) lv_label_set_text(_routes[i].lbl_sub, _routes[i].current_sub.c_str());
    }

    // 2. Request from Android Companion App via BLE (works anywhere, even outside!)
    ble_manager_send_media_command("TRANSIT_REQ");
}

bool AppTransit::run() {
    ESP_LOGI(TAG, "AppTransit::run()");
    if (_bg_obj != nullptr) {
        lv_obj_del(_bg_obj);
        _bg_obj = nullptr;
    }

    _bg_obj = lv_obj_create(lv_scr_act());
    lv_obj_set_size(_bg_obj, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(_bg_obj, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(_bg_obj, 0, 0);
    lv_obj_set_style_radius(_bg_obj, 0, 0);
    lv_obj_clear_flag(_bg_obj, LV_OBJ_FLAG_SCROLLABLE);

    // Top Header: Title
    _lbl_title = lv_label_create(_bg_obj);
    lv_obj_set_style_text_font(_lbl_title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(_lbl_title, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(_lbl_title, "ULASIM TAKIP");
    lv_obj_align(_lbl_title, LV_ALIGN_TOP_LEFT, 20, 15);

    // Top Header: Subtitle status
    _lbl_status = lv_label_create(_bg_obj);
    lv_obj_set_style_text_font(_lbl_status, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_lbl_status, lv_color_hex(0x9E9E9E), 0);
    lv_label_set_text(_lbl_status, "Yenilemek icin dokunun");
    lv_obj_align(_lbl_status, LV_ALIGN_TOP_LEFT, 20, 40);

    // Top Header: Refresh Button
    _btn_refresh = lv_btn_create(_bg_obj);
    lv_obj_set_size(_btn_refresh, 44, 44);
    lv_obj_align(_btn_refresh, LV_ALIGN_TOP_RIGHT, -20, 15);
    lv_obj_set_style_radius(_btn_refresh, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(_btn_refresh, lv_color_hex(0x222222), 0);
    lv_obj_set_style_border_color(_btn_refresh, lv_color_hex(0x444444), 0);
    lv_obj_set_style_border_width(_btn_refresh, 1, 0);
    lv_obj_add_event_cb(_btn_refresh, on_refresh_clicked, LV_EVENT_CLICKED, this);

    _lbl_refresh = lv_label_create(_btn_refresh);
    lv_obj_set_style_text_font(_lbl_refresh, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(_lbl_refresh, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(_lbl_refresh, LV_SYMBOL_REFRESH);
    lv_obj_center(_lbl_refresh);

    // Cards Scrollable Container
    lv_obj_t* list = lv_obj_create(_bg_obj);
    lv_obj_set_size(list, 380, 420);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_row(list, 10, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Create 4 Route Cards
    for (int i = 0; i < 4; i++) {
        lv_obj_t* card = lv_obj_create(list);
        lv_obj_set_size(card, 370, 92);
        lv_obj_set_style_bg_color(card, lv_color_hex(0x141414), 0);
        lv_obj_set_style_border_color(card, lv_color_hex(0x282828), 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_radius(card, 16, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        // Badge (Left)
        lv_obj_t* badge = lv_obj_create(card);
        lv_obj_set_size(badge, 54, 38);
        lv_obj_align(badge, LV_ALIGN_LEFT_MID, 5, 0);
        lv_obj_set_style_bg_color(badge, _routes[i].badge_color, 0);
        lv_obj_set_style_border_width(badge, 0, 0);
        lv_obj_set_style_radius(badge, 10, 0);
        lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* badge_lbl = lv_label_create(badge);
        lv_obj_set_style_text_font(badge_lbl, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(badge_lbl, lv_color_hex(0xFFFFFF), 0);
        lv_label_set_text(badge_lbl, _routes[i].badge_name);
        lv_obj_center(badge_lbl);

        // Route Name & Direction (Middle)
        lv_obj_t* route_lbl = lv_label_create(card);
        lv_obj_set_style_text_font(route_lbl, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(route_lbl, lv_color_hex(0xFFFFFF), 0);
        lv_label_set_text(route_lbl, _routes[i].route_name);
        lv_obj_align(route_lbl, LV_ALIGN_LEFT_MID, 70, -12);

        lv_obj_t* dir_lbl = lv_label_create(card);
        lv_obj_set_style_text_font(dir_lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(dir_lbl, lv_color_hex(0x888888), 0);
        lv_label_set_text(dir_lbl, _routes[i].direction);
        lv_obj_align(dir_lbl, LV_ALIGN_LEFT_MID, 70, 12);

        // Time (Right)
        _routes[i].lbl_time = lv_label_create(card);
        lv_obj_set_style_text_font(_routes[i].lbl_time, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(_routes[i].lbl_time, lv_color_hex(0x00E676), 0); // Vibrant Green
        lv_label_set_text(_routes[i].lbl_time, _routes[i].current_time.c_str());
        lv_obj_align(_routes[i].lbl_time, LV_ALIGN_RIGHT_MID, -10, -10);

        _routes[i].lbl_sub = lv_label_create(card);
        lv_obj_set_style_text_font(_routes[i].lbl_sub, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(_routes[i].lbl_sub, lv_color_hex(0xAAAAAA), 0);
        lv_label_set_text(_routes[i].lbl_sub, _routes[i].current_sub.c_str());
        lv_obj_align(_routes[i].lbl_sub, LV_ALIGN_RIGHT_MID, -10, 12);

        _routes[i].card_obj = card;
    }

    // Request refresh automatically upon opening
    request_refresh();

    return true;
}

bool AppTransit::back() {
    ESP_LOGI(TAG, "AppTransit::back() -> notifying core closed");
    return notifyCoreClosed();
}

void AppTransit::force_close() {
    if (_instance) {
        _instance->notifyCoreClosed();
    }
}

bool AppTransit::close() {
    ESP_LOGI(TAG, "AppTransit::close()");
    if (_bg_obj != nullptr) {
        lv_obj_del(_bg_obj);
        _bg_obj = nullptr;
    }
    _lbl_title = nullptr;
    _lbl_status = nullptr;
    _btn_refresh = nullptr;
    _lbl_refresh = nullptr;
    for (int i = 0; i < 4; i++) {
        _routes[i].card_obj = nullptr;
        _routes[i].lbl_time = nullptr;
        _routes[i].lbl_sub = nullptr;
    }
    _is_fetching = false;
    return true;
}

void AppTransit::update_route(int index, const char* time_str, const char* sub_str) {
    if (index < 0 || index >= 4) return;
    if (time_str) _routes[index].current_time = time_str;
    if (sub_str) _routes[index].current_sub = sub_str;

    if (_bg_obj && display_manager_is_on()) {
        if (_routes[index].lbl_time && time_str) {
            lv_label_set_text(_routes[index].lbl_time, time_str);
        }
        if (_routes[index].lbl_sub && sub_str) {
            lv_label_set_text(_routes[index].lbl_sub, sub_str);
        }
        if (_lbl_status) {
            lv_label_set_text(_lbl_status, "Canli Veri (Guncel)");
            lv_obj_set_style_text_color(_lbl_status, lv_color_hex(0x00E676), 0);
        }
    }
}

extern "C" void app_transit_update_from_ble(const char* payload) {
    // Expected payload format:
    // TRANSIT_RESP|55T_FENER:4 dk|M2_OSMANBEY:3 dk|T5_KUCUKPAZAR:5 dk|55T_HARBIYE:12 dk
    if (!payload) return;
    ESP_LOGI(TAG, "BLE Transit payload received: %s", payload);

    char buf[256];
    strncpy(buf, payload, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char* token = strtok(buf, "|");
    int index = 0;
    char* times[4] = {nullptr};
    while (token != NULL && index < 4) {
        char* colon = strchr(token, ':');
        if (colon) {
            *colon = '\0';
            times[index] = colon + 1;
        }
        token = strtok(NULL, "|");
        index++;
    }

    if (AppTransit::get_instance()) {
        bool is_disp_on = display_manager_is_on();
        if (!is_disp_on) {
            // Display is asleep -> update in-memory cache ONLY, DO NOT touch LVGL!
            for (int i = 0; i < 4; i++) {
                if (times[i]) {
                    AppTransit::get_instance()->update_route(i, times[i], "Canli Takip");
                }
            }
        } else if (bsp_display_lock(500)) {
            for (int i = 0; i < 4; i++) {
                if (times[i]) {
                    AppTransit::get_instance()->update_route(i, times[i], "Canli Takip");
                }
            }
            bsp_display_unlock();
        }
    }
}
