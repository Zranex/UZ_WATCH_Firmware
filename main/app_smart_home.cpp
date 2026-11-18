#include "app_smart_home.hpp"
extern const lv_image_dsc_t icon_smarthome;
#include "esp_lib_utils.h"
#include "bsp_board_extra.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>

extern "C" {
#include "ble_manager.h"
#include "wifi_manager.h"
#include "display_manager.h"
}

using namespace esp_brookesia;

#define ESPHOME_SALON_IP   "192.168.1.102"
#define ESPHOME_SALON_PATH "/switch/salon_isigi/toggle"

struct SmartBtnData {
    const char* cmd;
    const char* name;
    lv_color_t color;
};

static SmartBtnData s_btn_data[] = {
    {"SMART_HOME|LIGHT_1_TOGGLE", "Salon Isigi", lv_color_hex(0xFFA000)}, // Amber
    {"SMART_HOME|PC_WAKE",        "PC Ac (WoL)", lv_color_hex(0x00E676)}, // Green
    {"SMART_HOME|PC_SLEEP",       "PC Uyut",     lv_color_hex(0xE53935)}  // Red
};

static void direct_esphome_toggle_task(void* pv) {
    if (!wifi_manager_is_connected()) {
        if (!wifi_manager_is_active()) {
            wifi_manager_start();
        } else {
            wifi_manager_connect_default();
        }
        for (int i = 0; i < 40 && !wifi_manager_is_connected(); i++) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        if (!wifi_manager_is_connected()) {
            ESP_UTILS_LOGW("Wi-Fi could not connect in time for ESPHome toggle");
            vTaskDelete(NULL);
            return;
        }
    }

    struct sockaddr_in dest_addr = {};
    dest_addr.sin_addr.s_addr = inet_addr(ESPHOME_SALON_IP);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(80);

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock >= 0) {
        struct timeval timeout = { .tv_sec = 2, .tv_usec = 0 };
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        if (connect(sock, (struct sockaddr*)&dest_addr, sizeof(dest_addr)) == 0) {
            const char* req =
                "POST " ESPHOME_SALON_PATH " HTTP/1.1\r\n"
                "Host: " ESPHOME_SALON_IP "\r\n"
                "Content-Length: 0\r\n"
                "Connection: close\r\n\r\n";
            send(sock, req, strlen(req), 0);
            char rx_buf[64];
            recv(sock, rx_buf, sizeof(rx_buf) - 1, 0);
            ESP_UTILS_LOGI("Direct Wi-Fi ESPHome toggle sent to %s", ESPHOME_SALON_IP);
        } else {
            ESP_UTILS_LOGW("Direct Wi-Fi connect to ESPHome (%s) failed", ESPHOME_SALON_IP);
        }
        close(sock);
    }
    vTaskDelete(NULL);
}

AppSmartHome::AppSmartHome() : App("Akilli Ev", &icon_smarthome, true) {
    _bg_obj = nullptr;
    _wifi_status_label = nullptr;
    _wifi_ip_label = nullptr;
    _wifi_btn = nullptr;
    _wifi_btn_label = nullptr;
    _feedback_label = nullptr;
    _timer = nullptr;
}

AppSmartHome::~AppSmartHome() {
}

void AppSmartHome::send_command(const char* cmd, const char* friendly_name) {
    ESP_UTILS_LOGI("SmartHome Command: %s (%s)", cmd, friendly_name);
    display_manager_reset_timer();
    bsp_vibrate_pulse(50);

    if (strcmp(cmd, "SMART_HOME|LIGHT_1_TOGGLE") == 0 && (wifi_manager_is_connected() || !ble_manager_is_connected())) {
        // Direct Watch -> ESPHome Wi-Fi control (auto-connects Wi-Fi if needed!)
        xTaskCreate(direct_esphome_toggle_task, "esphome_http", 4096, NULL, 4, NULL);
        if (_feedback_label) {
            if (wifi_manager_is_connected()) {
                lv_label_set_text(_feedback_label, "Salon Isigi (Wi-Fi) Tetiklendi!");
            } else {
                lv_label_set_text(_feedback_label, "Wi-Fi Baglanip Tetikleniyor...");
            }
            lv_obj_set_style_text_color(_feedback_label, lv_color_hex(0x00E676), 0);
        }
        return;
    }

    // Send via BLE to Companion App (bridge to ESPHome / Wake-on-LAN / PC)
    ble_manager_send_media_command(cmd);

    if (_feedback_label) {
        lv_label_set_text_fmt(_feedback_label, "%s Gonderildi!", friendly_name);
        lv_obj_set_style_text_color(_feedback_label, lv_color_hex(0x1DB954), 0);
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
        lv_label_set_text_fmt(_wifi_ip_label, "IP: %s (Bagli)", strlen(ip) > 0 ? ip : "Hazir");
        lv_obj_set_style_text_color(_wifi_ip_label, lv_color_hex(0xCCCCCC), 0);
        lv_label_set_text(_wifi_btn_label, "Kapat");
        if (_wifi_btn) lv_obj_set_style_bg_color(_wifi_btn, lv_color_hex(0xD32F2F), 0);
    } else if (state == WIFI_STATE_CONNECTING) {
        lv_label_set_text(_wifi_status_label, LV_SYMBOL_REFRESH " Baglaniyor...");
        lv_obj_set_style_text_color(_wifi_status_label, lv_color_hex(0xFFA000), 0);
        lv_label_set_text(_wifi_ip_label, "TurkTelekom_ZYA41B...");
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
        lv_label_set_text(_wifi_ip_label, "Otomatik baglanmak icin acin");
        lv_obj_set_style_text_color(_wifi_ip_label, lv_color_hex(0x9E9E9E), 0);
        lv_label_set_text(_wifi_btn_label, "Ac");
        if (_wifi_btn) lv_obj_set_style_bg_color(_wifi_btn, lv_color_hex(0x1976D2), 0);
    }
}

void AppSmartHome::on_wifi_toggle_clicked(lv_event_t* e) {
    AppSmartHome* app = (AppSmartHome*)lv_event_get_user_data(e);
    if (!app) return;

    if (!wifi_manager_is_active()) {
        ESP_UTILS_LOGI("Starting Wi-Fi and auto-connecting to TurkTelekom");
        wifi_manager_start();
    } else if (wifi_manager_get_state() == WIFI_STATE_FAILED) {
        ESP_UTILS_LOGI("Retrying connection to TurkTelekom");
        wifi_manager_connect_default();
    } else if (wifi_manager_get_state() == WIFI_STATE_IDLE) {
        ESP_UTILS_LOGI("Connecting to TurkTelekom from IDLE");
        wifi_manager_connect_default();
    } else {
        ESP_UTILS_LOGI("Stopping Wi-Fi from SmartHome UI");
        wifi_manager_stop();
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
        lv_obj_set_style_bg_color(btn, data->color, 0);
        app->send_command(data->cmd, data->name);
    }
}

static lv_obj_t* create_smart_card(lv_obj_t* parent, int y, SmartBtnData* data, const char* subtitle, const char* symbol, lv_event_cb_t cb, void* user_data) {
    lv_obj_t* btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 370, 95);
    lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x181818), 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x333333), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_radius(btn, 18, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_shadow_spread(btn, 0, 0);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_user_data(btn, data);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);

    // Icon Circle Badge
    lv_obj_t* icon_bg = lv_obj_create(btn);
    lv_obj_set_size(icon_bg, 54, 54);
    lv_obj_align(icon_bg, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_set_style_radius(icon_bg, 27, 0);
    lv_obj_set_style_bg_color(icon_bg, lv_color_hex(0x282828), 0);
    lv_obj_set_style_border_width(icon_bg, 0, 0);
    lv_obj_clear_flag(icon_bg, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* icon = lv_label_create(icon_bg);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(icon, data->color, 0);
    lv_label_set_text(icon, symbol);
    lv_obj_center(icon);

    // Title & Subtitle
    lv_obj_t* title = lv_label_create(btn);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(title, data->name);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 72, -12);

    lv_obj_t* sub = lv_label_create(btn);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(0x9E9E9E), 0);
    lv_label_set_text(sub, subtitle);
    lv_obj_align(sub, LV_ALIGN_LEFT_MID, 72, 12);

    // Right Arrow
    lv_obj_t* arrow = lv_label_create(btn);
    lv_obj_set_style_text_font(arrow, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(arrow, lv_color_hex(0x666666), 0);
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -5, 0);

    return btn;
}

bool AppSmartHome::run() {
    ESP_UTILS_LOGI("AppSmartHome run - 3 Compact Cards & Wi-Fi UI");
    if (_bg_obj != nullptr) {
        return true;
    }

    display_manager_set_timeout(30000);

    _bg_obj = lv_obj_create(lv_scr_act());
    lv_obj_set_size(_bg_obj, LV_PCT(100), LV_PCT(100));
    lv_obj_center(_bg_obj);
    lv_obj_set_style_bg_color(_bg_obj, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(_bg_obj, 0, 0);
    lv_obj_set_style_radius(_bg_obj, 0, 0);
    lv_obj_clear_flag(_bg_obj, LV_OBJ_FLAG_SCROLLABLE);

    // 1. Top Title
    lv_obj_t* title = lv_label_create(_bg_obj);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(title, "AKILLI EV & PC KONTROL");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    // 2. Wi-Fi Status & Control Card (Height 74, Width 370)
    lv_obj_t* wifi_card = lv_obj_create(_bg_obj);
    lv_obj_set_size(wifi_card, 370, 74);
    lv_obj_align(wifi_card, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_set_style_bg_color(wifi_card, lv_color_hex(0x161616), 0);
    lv_obj_set_style_border_color(wifi_card, lv_color_hex(0x2C2C2C), 0);
    lv_obj_set_style_border_width(wifi_card, 1, 0);
    lv_obj_set_style_radius(wifi_card, 14, 0);
    lv_obj_clear_flag(wifi_card, LV_OBJ_FLAG_SCROLLABLE);

    _wifi_status_label = lv_label_create(wifi_card);
    lv_obj_set_style_text_font(_wifi_status_label, &lv_font_montserrat_16, 0);
    lv_obj_align(_wifi_status_label, LV_ALIGN_LEFT_MID, 5, -10);

    _wifi_ip_label = lv_label_create(wifi_card);
    lv_obj_set_style_text_font(_wifi_ip_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_wifi_ip_label, lv_color_hex(0x9E9E9E), 0);
    lv_obj_align(_wifi_ip_label, LV_ALIGN_LEFT_MID, 5, 12);

    _wifi_btn = lv_btn_create(wifi_card);
    lv_obj_set_size(_wifi_btn, 80, 44);
    lv_obj_align(_wifi_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_radius(_wifi_btn, 10, 0);
    lv_obj_set_style_shadow_width(_wifi_btn, 0, 0);
    lv_obj_set_style_shadow_spread(_wifi_btn, 0, 0);
    lv_obj_set_style_shadow_opa(_wifi_btn, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(_wifi_btn, on_wifi_toggle_clicked, LV_EVENT_CLICKED, this);

    _wifi_btn_label = lv_label_create(_wifi_btn);
    lv_obj_set_style_text_font(_wifi_btn_label, &lv_font_montserrat_14, 0);
    lv_obj_center(_wifi_btn_label);

    update_wifi_ui();

    // 3. Feedback Banner
    _feedback_label = lv_label_create(_bg_obj);
    lv_obj_set_style_text_font(_feedback_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_feedback_label, lv_color_hex(0x757575), 0);
    lv_label_set_text(_feedback_label, "Komut gondermek icin dokunun");
    lv_obj_align(_feedback_label, LV_ALIGN_TOP_MID, 0, 116);

    // 4. Card 1: Tavan Isigi (Y = 138)
    create_smart_card(_bg_obj, 138, &s_btn_data[0], "Dokun: Ac / Kapat", LV_SYMBOL_IMAGE, on_btn_clicked, this);

    // 5. Card 2: PC Ac (WoL) (Y = 244)
    create_smart_card(_bg_obj, 244, &s_btn_data[1], "Sunucusuz: Wake-on-LAN", LV_SYMBOL_PLAY, on_btn_clicked, this);

    // 6. Card 3: PC Uyut (Y = 350)
    create_smart_card(_bg_obj, 350, &s_btn_data[2], "Dokun: Uyku Modu", LV_SYMBOL_POWER, on_btn_clicked, this);

    // 7. Timer: Update Wi-Fi status every 1 second
    _timer = lv_timer_create(on_timer_tick, 1000, this);

    return true;
}

bool AppSmartHome::back() {
    return notifyCoreClosed();
}

bool AppSmartHome::close() {
    ESP_UTILS_LOGI("AppSmartHome close");
    display_manager_set_timeout(5000);
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
