#include "app_smart_home.hpp"
#include "pc_control.hpp"
#include "tv_control.hpp"
extern const lv_image_dsc_t icon_smarthome;
#include "esp_lib_utils.h"
#include "bsp_board_extra.h"
#include <string.h>

extern "C" {
#include "ble_manager.h"
#include "wifi_manager.h"
#include "display_manager.h"
}

using namespace esp_brookesia;

struct SmartBtnData {
    const char* cmd;
    const char* name;
    uint32_t color_hex;
};

static SmartBtnData s_actions[] = {
    {"LIGHT_TOGGLE",        "Salon Isigi",      0xFFA000}, // 0
    {"PC_WAKE",             "PC Ac",            0x00E676}, // 1
    {"PC_LOCK",             "Kilitle",          0x42A5F5}, // 2
    {"PC_SLEEP",            "Uyut",             0xEF5350}, // 3
    {"PC_MEDIA_PREV",       LV_SYMBOL_PREV,     0xB0BEC5}, // 4
    {"PC_MEDIA_PLAY_PAUSE", LV_SYMBOL_PAUSE " / " LV_SYMBOL_PLAY, 0x00E5FF}, // 5
    {"PC_MEDIA_NEXT",       LV_SYMBOL_NEXT,     0xB0BEC5}, // 6
    {"PC_VOL_DOWN",         "Ses -",            0x90CAF9}, // 7
    {"PC_VOL_MUTE",         LV_SYMBOL_MUTE,     0xFF7043}, // 8
    {"PC_VOL_UP",           "Ses +",            0x66BB6A}, // 9
    {"PC_OPEN_SPOTIFY",     "Spotify",          0x1DB954}, // 10
    {"PC_OPEN_YOUTUBE",     "YouTube",          0xFF1744}, // 11
    {"PC_OPEN_CHROME",      "Chrome",           0x42A5F5}, // 12
    {"COMBO_BREAK",         "Mutfak Molasi",    0xFFB300}, // 13
    {"COMBO_MOVIE",         "Sinema Modu",      0xAB47BC}, // 14
    {"PC_MODE_GAME",        "Oyun Modu",        0x26C6DA}, // 15
    {"PC_BOSS_KEY",         "Panik (Gizle)",    0xD32F2F}, // 16
    {"PC_SEEK_BACK",        "<< 10sn",          0x80CBC4}, // 17
    {"PC_FULLSCREEN",       "Tam Ekran",        0xCE93D8}, // 18
    {"PC_SEEK_FWD",         "10sn >>",          0x80CBC4}, // 19
    {"PC_SCREENSHOT",       "Ekran Grnt",       0xFFD54F}, // 20
    {"PC_MINIMIZE_ALL",     "Masaustu",         0x90CAF9}, // 21
    {"PC_CLOSE_TAB",        "Sekme Kpt",        0xEF9A9A}, // 22
    // TV Remote (Tivibu AndroidTV 192.168.1.159:6466)
    {"TV_POWER",            "TV Guc",           0xEF5350}, // 23
    {"TV_PLAY_PAUSE",       "Oynat/Dur",        0x00E5FF}, // 24
    {"TV_MUTE",             "Sessiz",           0xFF7043}, // 25
    {"TV_VOL_DOWN_1",       "Ses -",            0x90CAF9}, // 26
    {"TV_CH_DOWN",          "Knl -",            0xB0BEC5}, // 27
    {"TV_CH_UP",            "Knl +",            0xB0BEC5}, // 28
    {"TV_VOL_UP_1",         "Ses +",            0x66BB6A}, // 29
    {"TV_BACK",             "Geri",             0xFFCA28}, // 30
    {"TV_UP",               LV_SYMBOL_UP,       0x00E5FF}, // 31
    {"TV_HOME",             "Ana Ekran",        0xAB47BC}, // 32
    {"TV_LEFT",             LV_SYMBOL_LEFT,     0x00E5FF}, // 33
    {"TV_OK",               "OK",               0x00E676}, // 34
    {"TV_RIGHT",            LV_SYMBOL_RIGHT,    0x00E5FF}, // 35
    {"TV_DOWN",             LV_SYMBOL_DOWN,     0x00E5FF}, // 36
    {"TV_OPEN_YOUTUBE",     "YouTube",          0xFF1744}, // 37
    {"TV_OPEN_NETFLIX",     "Netflix",          0xE50914}, // 38
};

AppSmartHome::AppSmartHome() : App("Akilli Ev", &icon_smarthome, true) {
    _bg_obj = nullptr;
    _wifi_status_label = nullptr;
    _wifi_ip_label = nullptr;
    _wifi_btn = nullptr;
    _wifi_btn_label = nullptr;
    _feedback_label = nullptr;
    _timer = nullptr;
    _wifi_init_timer = nullptr;
}

AppSmartHome::~AppSmartHome() {
}

void AppSmartHome::send_command(const char* cmd, const char* friendly_name) {
    ESP_UTILS_LOGI("SmartHome/PC Command: %s (%s)", cmd, friendly_name);
    display_manager_reset_timer();
    bsp_vibrate_pulse(45);

    if (strncmp(cmd, "TV_", 3) == 0) {
        tv_control_send_async(cmd);
    } else {
        pc_control_send_async(cmd);
    }

    if (_feedback_label) {
        if (wifi_manager_is_connected()) {
            lv_label_set_text_fmt(_feedback_label, "%s (Wi-Fi LAN) Gonderildi!", friendly_name);
            lv_obj_set_style_text_color(_feedback_label, lv_color_hex(0x00E676), 0);
        } else {
            lv_label_set_text_fmt(_feedback_label, "Wi-Fi Baglanip %s Gonderiliyor...", friendly_name);
            lv_obj_set_style_text_color(_feedback_label, lv_color_hex(0xFFA000), 0);
        }
    }
}

void AppSmartHome::update_wifi_ui() {
    if (!_wifi_status_label || !_wifi_ip_label || !_wifi_btn_label) {
        return;
    }

    bool active = wifi_manager_is_active();
    bool connected = wifi_manager_is_connected();
    wifi_state_t state = wifi_manager_get_state();

    if (connected) {
        char ip[32] = {0};
        wifi_manager_get_ip(ip);
        lv_label_set_text(_wifi_status_label, LV_SYMBOL_WIFI " TurkTelekom");
        lv_obj_set_style_text_color(_wifi_status_label, lv_color_hex(0x4CAF50), 0);
        lv_label_set_text_fmt(_wifi_ip_label, "PC: %s | Saat: %s", pc_control_get_cached_ip(), strlen(ip) > 0 ? ip : "OK");
        lv_obj_set_style_text_color(_wifi_ip_label, lv_color_hex(0xB0BEC5), 0);
        lv_label_set_text(_wifi_btn_label, "Kapat");
        if (_wifi_btn) lv_obj_set_style_bg_color(_wifi_btn, lv_color_hex(0xD32F2F), 0);
    } else if (state == WIFI_STATE_CONNECTING) {
        lv_label_set_text(_wifi_status_label, LV_SYMBOL_REFRESH " Baglaniyor...");
        lv_obj_set_style_text_color(_wifi_status_label, lv_color_hex(0xFFA000), 0);
        lv_label_set_text(_wifi_ip_label, "YOUR_WIFI_SSID");
        lv_obj_set_style_text_color(_wifi_ip_label, lv_color_hex(0x9E9E9E), 0);
        lv_label_set_text(_wifi_btn_label, "Kapat");
        if (_wifi_btn) lv_obj_set_style_bg_color(_wifi_btn, lv_color_hex(0x424242), 0);
    } else if (state == WIFI_STATE_FAILED) {
        lv_label_set_text(_wifi_status_label, LV_SYMBOL_CLOSE " Baglanamadi!");
        lv_obj_set_style_text_color(_wifi_status_label, lv_color_hex(0xFF5252), 0);
        lv_label_set_text(_wifi_ip_label, "Aga ulasilamadi. Tekrar dene.");
        lv_obj_set_style_text_color(_wifi_ip_label, lv_color_hex(0xE57373), 0);
        lv_label_set_text(_wifi_btn_label, "Tekrar");
        if (_wifi_btn) lv_obj_set_style_bg_color(_wifi_btn, lv_color_hex(0xFF9800), 0);
    } else if (active) {
        lv_label_set_text(_wifi_status_label, LV_SYMBOL_WIFI " Wi-Fi: Acik");
        lv_obj_set_style_text_color(_wifi_status_label, lv_color_hex(0x00E5FF), 0);
        lv_label_set_text(_wifi_ip_label, "Baglanmak icin dokunun");
        lv_obj_set_style_text_color(_wifi_ip_label, lv_color_hex(0x9E9E9E), 0);
        lv_label_set_text(_wifi_btn_label, "Baglan");
        if (_wifi_btn) lv_obj_set_style_bg_color(_wifi_btn, lv_color_hex(0x1976D2), 0);
    } else {
        lv_label_set_text(_wifi_status_label, LV_SYMBOL_WARNING " Wi-Fi: Kapali");
        lv_obj_set_style_text_color(_wifi_status_label, lv_color_hex(0x888888), 0);
        lv_label_set_text(_wifi_ip_label, "Direkt PC/Isik icin Wi-Fi acin");
        lv_obj_set_style_text_color(_wifi_ip_label, lv_color_hex(0x9E9E9E), 0);
        lv_label_set_text(_wifi_btn_label, "Ac");
        if (_wifi_btn) lv_obj_set_style_bg_color(_wifi_btn, lv_color_hex(0x1976D2), 0);
    }
}

void AppSmartHome::on_wifi_toggle_clicked(lv_event_t* e) {
    AppSmartHome* app = (AppSmartHome*)lv_event_get_user_data(e);
    if (!app) return;

    if (!wifi_manager_is_active()) {
        wifi_manager_start_async();
    } else if (wifi_manager_get_state() == WIFI_STATE_FAILED || wifi_manager_get_state() == WIFI_STATE_IDLE) {
        wifi_manager_connect_default_async();
    } else {
        wifi_manager_stop_async();
    }
    app->update_wifi_ui();
}

void AppSmartHome::on_timer_tick(lv_timer_t* t) {
    AppSmartHome* app = (AppSmartHome*)lv_timer_get_user_data(t);
    if (app) {
        app->update_wifi_ui();
    }
}

void AppSmartHome::on_btn_clicked(lv_event_t* e) {
    AppSmartHome* app = (AppSmartHome*)lv_event_get_user_data(e);
    lv_obj_t* btn = (lv_obj_t*)lv_event_get_target(e);
    SmartBtnData* data = (SmartBtnData*)lv_obj_get_user_data(btn);

    if (app && data) {
        app->send_command(data->cmd, data->name);
    }
}

static lv_style_t s_style_row;
static lv_style_t s_style_btn;
static bool s_styles_ready = false;

static void init_shared_styles() {
    if (s_styles_ready) return;
    lv_style_init(&s_style_row);
    lv_style_set_bg_opa(&s_style_row, LV_OPA_TRANSP);
    lv_style_set_border_width(&s_style_row, 0);
    lv_style_set_pad_all(&s_style_row, 0);
    lv_style_set_pad_column(&s_style_row, 8);
    lv_style_set_shadow_width(&s_style_row, 0);

    lv_style_init(&s_style_btn);
    lv_style_set_radius(&s_style_btn, 14);
    lv_style_set_bg_color(&s_style_btn, lv_color_hex(0x181C24));
    lv_style_set_border_width(&s_style_btn, 1);
    lv_style_set_pad_all(&s_style_btn, 4);
    lv_style_set_shadow_width(&s_style_btn, 0);
    s_styles_ready = true;
}

static void style_no_shadow(lv_obj_t* obj) {
    lv_obj_set_style_shadow_width(obj, 0, 0);
}

static lv_obj_t* create_row(lv_obj_t* parent, int height) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_add_style(row, &s_style_row, 0);
    lv_obj_set_size(row, 374, height);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

static lv_obj_t* create_grid_btn(lv_obj_t* row, SmartBtnData* data, int height, lv_event_cb_t cb, void* user_data) {
    lv_obj_t* btn = lv_btn_create(row);
    lv_obj_add_style(btn, &s_style_btn, 0);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_height(btn, height);
    lv_obj_set_style_border_color(btn, lv_color_hex(data->color_hex), 0);
    lv_obj_set_user_data(btn, data);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);

    lv_obj_t* lbl = lv_label_create(btn);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(data->color_hex), 0);
    lv_label_set_text(lbl, data->name);
    lv_obj_center(lbl);
    return btn;
}

static void add_section_hdr(lv_obj_t* parent, const char* text) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_width(l, 370);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0x78909C), 0);
    lv_obj_set_style_pad_top(l, 4, 0);
    lv_label_set_text(l, text);
}

bool AppSmartHome::run() {
    ESP_UTILS_LOGI("AppSmartHome run - Full Smart Home + PC Control Hub");
    if (_bg_obj != nullptr) {
        return true;
    }

    init_shared_styles();
    display_manager_set_timeout(45000);

    _bg_obj = lv_obj_create(lv_scr_act());
    lv_obj_set_size(_bg_obj, LV_PCT(100), LV_PCT(100));
    lv_obj_center(_bg_obj);
    lv_obj_set_style_bg_color(_bg_obj, lv_color_hex(0x06080D), 0);
    lv_obj_set_style_border_width(_bg_obj, 0, 0);
    lv_obj_set_style_radius(_bg_obj, 0, 0);
    lv_obj_set_style_pad_top( _bg_obj, 12, 0);
    lv_obj_set_style_pad_bottom(_bg_obj, 36, 0);
    lv_obj_set_style_pad_hor(_bg_obj, 0, 0);
    lv_obj_set_style_pad_row(_bg_obj, 8, 0);
    lv_obj_set_flex_flow(_bg_obj, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(_bg_obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(_bg_obj, LV_SCROLLBAR_MODE_OFF);
    style_no_shadow(_bg_obj);

    // 1. Top Title
    lv_obj_t* title = lv_label_create(_bg_obj);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(title, "AKILLI EV // TV & PC");

    // 2. Wi-Fi Status & Control Card
    lv_obj_t* wifi_card = lv_obj_create(_bg_obj);
    lv_obj_set_size(wifi_card, 374, 70);
    lv_obj_set_style_bg_color(wifi_card, lv_color_hex(0x12161F), 0);
    lv_obj_set_style_border_color(wifi_card, lv_color_hex(0x263238), 0);
    lv_obj_set_style_border_width(wifi_card, 1, 0);
    lv_obj_set_style_radius(wifi_card, 14, 0);
    lv_obj_set_style_pad_hor(wifi_card, 12, 0);
    lv_obj_clear_flag(wifi_card, LV_OBJ_FLAG_SCROLLABLE);
    style_no_shadow(wifi_card);

    _wifi_status_label = lv_label_create(wifi_card);
    lv_obj_set_style_text_font(_wifi_status_label, &lv_font_montserrat_16, 0);
    lv_obj_align(_wifi_status_label, LV_ALIGN_LEFT_MID, 0, -10);

    _wifi_ip_label = lv_label_create(wifi_card);
    lv_obj_set_style_text_font(_wifi_ip_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_wifi_ip_label, lv_color_hex(0x9E9E9E), 0);
    lv_obj_align(_wifi_ip_label, LV_ALIGN_LEFT_MID, 0, 12);

    _wifi_btn = lv_btn_create(wifi_card);
    lv_obj_set_size(_wifi_btn, 76, 42);
    lv_obj_align(_wifi_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_radius(_wifi_btn, 10, 0);
    style_no_shadow(_wifi_btn);
    lv_obj_add_event_cb(_wifi_btn, on_wifi_toggle_clicked, LV_EVENT_CLICKED, this);

    _wifi_btn_label = lv_label_create(_wifi_btn);
    lv_obj_set_style_text_font(_wifi_btn_label, &lv_font_montserrat_14, 0);
    lv_obj_center(_wifi_btn_label);

    update_wifi_ui();

    // 3. Feedback Banner
    _feedback_label = lv_label_create(_bg_obj);
    lv_obj_set_style_text_font(_feedback_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_feedback_label, lv_color_hex(0x78909C), 0);
    lv_label_set_text(_feedback_label, "Asagi kaydir: PC Medya, Uygulama ve Modlar");

    // 4. Salon Light Hero Button
    lv_obj_t* row_light = create_row(_bg_obj, 54);
    create_grid_btn(row_light, &s_actions[0], 52, on_btn_clicked, this);

    // 5. Tivibu AndroidTV Direct Remote (192.168.1.159:6466)
    add_section_hdr(_bg_obj, "TELEVIZYON KUMANDA (TIVIBU TV)");
    lv_obj_t* row_tv1 = create_row(_bg_obj, 48);
    create_grid_btn(row_tv1, &s_actions[23], 46, on_btn_clicked, this);
    create_grid_btn(row_tv1, &s_actions[24], 46, on_btn_clicked, this);
    create_grid_btn(row_tv1, &s_actions[25], 46, on_btn_clicked, this);

    lv_obj_t* row_tv2 = create_row(_bg_obj, 46);
    create_grid_btn(row_tv2, &s_actions[26], 44, on_btn_clicked, this);
    create_grid_btn(row_tv2, &s_actions[27], 44, on_btn_clicked, this);
    create_grid_btn(row_tv2, &s_actions[28], 44, on_btn_clicked, this);
    create_grid_btn(row_tv2, &s_actions[29], 44, on_btn_clicked, this);

    lv_obj_t* row_tv3 = create_row(_bg_obj, 46);
    create_grid_btn(row_tv3, &s_actions[30], 44, on_btn_clicked, this);
    create_grid_btn(row_tv3, &s_actions[31], 44, on_btn_clicked, this);
    create_grid_btn(row_tv3, &s_actions[32], 44, on_btn_clicked, this);

    lv_obj_t* row_tv4 = create_row(_bg_obj, 48);
    create_grid_btn(row_tv4, &s_actions[33], 46, on_btn_clicked, this);
    create_grid_btn(row_tv4, &s_actions[34], 46, on_btn_clicked, this);
    create_grid_btn(row_tv4, &s_actions[35], 46, on_btn_clicked, this);

    lv_obj_t* row_tv5 = create_row(_bg_obj, 46);
    create_grid_btn(row_tv5, &s_actions[36], 44, on_btn_clicked, this);
    create_grid_btn(row_tv5, &s_actions[37], 44, on_btn_clicked, this);
    create_grid_btn(row_tv5, &s_actions[38], 44, on_btn_clicked, this);

    // 6. PC Power Row (Wake / Lock / Sleep)
    add_section_hdr(_bg_obj, "BILGISAYAR GUC (192.168.1.193)");
    lv_obj_t* row_pwr = create_row(_bg_obj, 52);
    create_grid_btn(row_pwr, &s_actions[1], 50, on_btn_clicked, this);
    create_grid_btn(row_pwr, &s_actions[2], 50, on_btn_clicked, this);
    create_grid_btn(row_pwr, &s_actions[3], 50, on_btn_clicked, this);

    // 6. PC Media & Volume Rows
    add_section_hdr(_bg_obj, "PC MEDYA & SES KONTROLU");
    lv_obj_t* row_med = create_row(_bg_obj, 50);
    create_grid_btn(row_med, &s_actions[4], 48, on_btn_clicked, this);
    create_grid_btn(row_med, &s_actions[5], 48, on_btn_clicked, this);
    create_grid_btn(row_med, &s_actions[6], 48, on_btn_clicked, this);

    lv_obj_t* row_vol = create_row(_bg_obj, 50);
    create_grid_btn(row_vol, &s_actions[7], 48, on_btn_clicked, this);
    create_grid_btn(row_vol, &s_actions[8], 48, on_btn_clicked, this);
    create_grid_btn(row_vol, &s_actions[9], 48, on_btn_clicked, this);

    lv_obj_t* row_vid = create_row(_bg_obj, 48);
    create_grid_btn(row_vid, &s_actions[17], 46, on_btn_clicked, this);
    create_grid_btn(row_vid, &s_actions[18], 46, on_btn_clicked, this);
    create_grid_btn(row_vid, &s_actions[19], 46, on_btn_clicked, this);

    lv_obj_t* row_sys = create_row(_bg_obj, 48);
    create_grid_btn(row_sys, &s_actions[20], 46, on_btn_clicked, this);
    create_grid_btn(row_sys, &s_actions[21], 46, on_btn_clicked, this);
    create_grid_btn(row_sys, &s_actions[22], 46, on_btn_clicked, this);

    // 7. PC Quick Apps
    add_section_hdr(_bg_obj, "HIZLI PC UYGULAMALARI");
    lv_obj_t* row_apps = create_row(_bg_obj, 50);
    create_grid_btn(row_apps, &s_actions[10], 48, on_btn_clicked, this);
    create_grid_btn(row_apps, &s_actions[11], 48, on_btn_clicked, this);
    create_grid_btn(row_apps, &s_actions[12], 48, on_btn_clicked, this);

    // 8. Panic / Boss Key Row
    lv_obj_t* row_panic = create_row(_bg_obj, 50);
    create_grid_btn(row_panic, &s_actions[16], 48, on_btn_clicked, this);

    _timer = lv_timer_create(on_timer_tick, 1000, this);
    // Defer Wi-Fi radio bring-up by 350ms AFTER UI renders so RF cal + UI alloc NEVER collide!
    _wifi_init_timer = lv_timer_create(on_deferred_wifi_init, 350, this);
    if (_wifi_init_timer) {
        lv_timer_set_repeat_count(_wifi_init_timer, 1);
    }
    return true;
}

void AppSmartHome::on_deferred_wifi_init(lv_timer_t* t) {
    AppSmartHome* app = (AppSmartHome*)lv_timer_get_user_data(t);
    if (!app) return;
    app->_wifi_init_timer = nullptr; // One-shot timer auto-deletes after this callback
    if (!app->_bg_obj) return;
    if (!wifi_manager_is_active() && !wifi_manager_is_ap_active()) {
        wifi_manager_start_async();
    } else if (wifi_manager_get_state() == WIFI_STATE_IDLE || wifi_manager_get_state() == WIFI_STATE_FAILED) {
        wifi_manager_connect_default_async();
    }
    app->update_wifi_ui();
}

bool AppSmartHome::back() {
    return notifyCoreClosed();
}

bool AppSmartHome::close() {
    ESP_UTILS_LOGI("AppSmartHome close");
    display_manager_set_timeout(5000);
    if (_wifi_init_timer) {
        lv_timer_delete(_wifi_init_timer);
        _wifi_init_timer = nullptr;
    }
    if (_timer) {
        lv_timer_delete(_timer);
        _timer = nullptr;
    }
    if (_bg_obj) {
        lv_obj_delete_async(_bg_obj);
        _bg_obj = nullptr;
        _wifi_status_label = nullptr;
        _wifi_ip_label = nullptr;
        _wifi_btn = nullptr;
        _wifi_btn_label = nullptr;
        _feedback_label = nullptr;
    }
    return true;
}
