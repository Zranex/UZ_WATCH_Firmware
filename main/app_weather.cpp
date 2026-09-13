#include "app_weather.hpp"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_http_client.h"
#include "cJSON.h"
#include "wifi_manager.h"

extern const lv_image_dsc_t icon_weather;

// C Wrapper for LVGL display lock
extern "C" bool bsp_display_lock(uint32_t timeout_ms);
extern "C" void bsp_display_unlock(void);

AppWeather* AppWeather::_instance = nullptr;

AppWeather::AppWeather() : esp_brookesia::systems::phone::App("Hava Durumu", &icon_weather, true) {
    _bg_obj = nullptr;
    _label_city = nullptr;
    _label_temp = nullptr;
    _label_condition = nullptr;
    _btn_refresh = nullptr;
    _label_refresh = nullptr;
    _is_fetching = false;
    _instance = this;
}

AppWeather::~AppWeather() {
    if (_instance == this) _instance = nullptr;
}

static void parse_day(const std::string& raw, std::string& max_min, std::string& code_str) {
    size_t p1 = raw.find('/');
    size_t p2 = raw.find('/', p1 + 1);
    if (p1 != std::string::npos && p2 != std::string::npos) {
        std::string mx = raw.substr(0, p1);
        std::string mn = raw.substr(p1 + 1, p2 - p1 - 1);
        code_str = raw.substr(p2 + 1);
        max_min = mx + " / " + mn;
    } else {
        max_min = "- / -";
        code_str = "0";
    }
}

static const char* short_cond(const std::string& code) {
    int c = atoi(code.c_str());
    if (c == 0) return "Gunesli";
    if (c <= 3) return "P.Bulutlu";
    if (c == 45 || c == 48) return "Sisli";
    if (c >= 51 && c <= 57) return "Cisenti";
    if (c >= 61 && c <= 67) return "Yagmur";
    if (c >= 71 && c <= 77) return "Kar";
    if (c >= 80 && c <= 82) return "Saganak";
    if (c >= 95) return "Firtina";
    return "?";
}

void AppWeather::on_refresh_clicked(lv_event_t* e) {
    AppWeather* app = (AppWeather*)lv_event_get_user_data(e);
    if (app) {
        app->fetch_weather_via_wifi();
    }
}

void AppWeather::fetch_weather_via_wifi() {
    if (_is_fetching) return;
    if (!wifi_manager_is_connected()) {
        if (_label_condition) {
            lv_label_set_text(_label_condition, "Wi-Fi Bagli Degil");
        }
        return;
    }
    _is_fetching = true;
    if (_label_condition) {
        lv_label_set_text(_label_condition, "Wi-Fi: Guncelleniyor...");
    }
    xTaskCreate(weather_http_task, "weather_fetch", 4096, this, 3, NULL);
}

void AppWeather::weather_http_task(void* pvParameters) {
    AppWeather* app = (AppWeather*)pvParameters;
    
    // Open-Meteo HTTP API (lat=41.01, lon=28.97 for Istanbul)
    const char* url = "http://api.open-meteo.com/v1/forecast?latitude=41.01&longitude=28.97&current=temperature_2m,weather_code&daily=weather_code,temperature_2m_max,temperature_2m_min&timezone=auto";
    
    esp_http_client_config_t config = {};
    config.url = url;
    config.timeout_ms = 6000;
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE("AppWeather", "HTTP client init failed");
        if (app) app->_is_fetching = false;
        vTaskDelete(NULL);
        return;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(client);
        char* buffer = (char*)malloc(2048);
        if (buffer) {
            int read_len = esp_http_client_read_response(client, buffer, 2047);
            if (read_len > 0) {
                buffer[read_len] = '\0';
                cJSON* root = cJSON_Parse(buffer);
                if (root) {
                    cJSON* current = cJSON_GetObjectItem(root, "current");
                    cJSON* daily = cJSON_GetObjectItem(root, "daily");
                    
                    char temp_str[16] = "--";
                    int current_code = 0;
                    if (current) {
                        cJSON* temp_item = cJSON_GetObjectItem(current, "temperature_2m");
                        cJSON* code_item = cJSON_GetObjectItem(current, "weather_code");
                        if (temp_item) snprintf(temp_str, sizeof(temp_str), "%.0f", temp_item->valuedouble);
                        if (code_item) current_code = code_item->valueint;
                    }
                    
                    char d0[32] = "- / -/0", d1[32] = "- / -/0", d2[32] = "- / -/0";
                    if (daily) {
                        cJSON* max_arr = cJSON_GetObjectItem(daily, "temperature_2m_max");
                        cJSON* min_arr = cJSON_GetObjectItem(daily, "temperature_2m_min");
                        cJSON* code_arr = cJSON_GetObjectItem(daily, "weather_code");
                        
                        auto get_day_str = [](cJSON* mx, cJSON* mn, cJSON* cd, int idx, char* out, size_t out_sz) {
                            double mx_v = 0, mn_v = 0;
                            int cd_v = 0;
                            if (mx && cJSON_GetArrayItem(mx, idx)) mx_v = cJSON_GetArrayItem(mx, idx)->valuedouble;
                            if (mn && cJSON_GetArrayItem(mn, idx)) mn_v = cJSON_GetArrayItem(mn, idx)->valuedouble;
                            if (cd && cJSON_GetArrayItem(cd, idx)) cd_v = cJSON_GetArrayItem(cd, idx)->valueint;
                            snprintf(out, out_sz, "%.0f/%.0f/%d", mx_v, mn_v, cd_v);
                        };
                        
                        get_day_str(max_arr, min_arr, code_arr, 0, d0, sizeof(d0));
                        get_day_str(max_arr, min_arr, code_arr, 1, d1, sizeof(d1));
                        get_day_str(max_arr, min_arr, code_arr, 2, d2, sizeof(d2));
                    }
                    
                    char code_str[8];
                    snprintf(code_str, sizeof(code_str), "%d", current_code);
                    const char* cond_name = short_cond(code_str);
                    
                    if (bsp_display_lock(1000)) {
                        if (AppWeather::get_instance()) {
                            AppWeather::get_instance()->update_advanced_weather("Istanbul (Wi-Fi)", temp_str, cond_name, d0, d1, d2);
                        }
                        bsp_display_unlock();
                    }
                    
                    cJSON_Delete(root);
                }
            }
            free(buffer);
        }
    } else {
        ESP_LOGE("AppWeather", "HTTP GET error: %s", esp_err_to_name(err));
        if (bsp_display_lock(1000)) {
            if (app && app->_label_condition) {
                lv_label_set_text(app->_label_condition, "Wi-Fi Hatasi");
            }
            bsp_display_unlock();
        }
    }

    esp_http_client_cleanup(client);
    if (app) app->_is_fetching = false;
    vTaskDelete(NULL);
}

bool AppWeather::run() {
    if (_bg_obj != nullptr) return true;

    _bg_obj = lv_obj_create(lv_scr_act());
    lv_obj_set_size(_bg_obj, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(_bg_obj, lv_color_hex(0x202124), 0);
    lv_obj_set_style_border_width(_bg_obj, 0, 0);
    lv_obj_set_style_radius(_bg_obj, 0, 0);
    lv_obj_clear_flag(_bg_obj, LV_OBJ_FLAG_SCROLLABLE);

    // Refresh Button (Top Right)
    _btn_refresh = lv_btn_create(_bg_obj);
    lv_obj_set_size(_btn_refresh, 46, 46);
    lv_obj_align(_btn_refresh, LV_ALIGN_TOP_RIGHT, -20, 45);
    lv_obj_set_style_radius(_btn_refresh, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(_btn_refresh, lv_color_hex(0x303134), 0);
    lv_obj_set_style_border_width(_btn_refresh, 0, 0);
    lv_obj_add_event_cb(_btn_refresh, on_refresh_clicked, LV_EVENT_CLICKED, this);

    _label_refresh = lv_label_create(_btn_refresh);
    lv_label_set_text(_label_refresh, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_color(_label_refresh, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(_label_refresh);

    // City Label
    _label_city = lv_label_create(_bg_obj);
    lv_obj_set_style_text_font(_label_city, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(_label_city, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(_label_city, _current_city.c_str());
    lv_obj_align(_label_city, LV_ALIGN_TOP_MID, 0, 50);

    // Temp Label
    _label_temp = lv_label_create(_bg_obj);
    lv_obj_set_style_text_font(_label_temp, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(_label_temp, lv_color_hex(0xFABD04), 0);
    lv_label_set_text_fmt(_label_temp, "%s C", _current_temp.c_str());
    lv_obj_align(_label_temp, LV_ALIGN_TOP_MID, 0, 95);

    // Condition Label
    _label_condition = lv_label_create(_bg_obj);
    lv_obj_set_style_text_font(_label_condition, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(_label_condition, lv_color_hex(0x9AA0A6), 0);
    lv_label_set_text(_label_condition, _current_condition.c_str());
    lv_obj_align(_label_condition, LV_ALIGN_TOP_MID, 0, 150);

    // Parse daily data
    std::string m0, c0, m1, c1, m2, c2;
    parse_day(_day0, m0, c0);
    parse_day(_day1, m1, c1);
    parse_day(_day2, m2, c2);

    const char* days[] = {"Bugun", "Yarin", "E.Gun"};
    std::string mm[] = {m0, m1, m2};
    std::string cc[] = {c0, c1, c2};
    int x_offsets[] = {-130, 0, 130};

    for(int i=0; i<3; i++) {
        lv_obj_t* card = lv_obj_create(_bg_obj);
        lv_obj_set_size(card, 110, 140);
        lv_obj_align(card, LV_ALIGN_CENTER, x_offsets[i], 80);
        lv_obj_set_style_bg_color(card, lv_color_hex(0x303134), 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_radius(card, 15, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* l_day = lv_label_create(card);
        lv_obj_set_style_text_font(l_day, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(l_day, lv_color_hex(0xFFFFFF), 0);
        lv_label_set_text(l_day, days[i]);
        lv_obj_align(l_day, LV_ALIGN_TOP_MID, 0, 5);

        lv_obj_t* l_icon = lv_label_create(card);
        lv_obj_set_style_text_font(l_icon, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(l_icon, lv_color_hex(0xFABD04), 0);
        lv_label_set_text(l_icon, short_cond(cc[i]));
        lv_obj_align(l_icon, LV_ALIGN_CENTER, 0, 0);

        lv_obj_t* l_mm = lv_label_create(card);
        lv_obj_set_style_text_font(l_mm, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(l_mm, lv_color_hex(0x9AA0A6), 0);
        lv_label_set_text(l_mm, mm[i].c_str());
        lv_obj_align(l_mm, LV_ALIGN_BOTTOM_MID, 0, -5);
    }

    // If Wi-Fi is connected, automatically initiate a fetch
    if (wifi_manager_is_connected()) {
        fetch_weather_via_wifi();
    }

    return true;
}

bool AppWeather::back() {
    return close();
}

bool AppWeather::close() {
    if (_bg_obj) {
        lv_obj_del(_bg_obj);
        _bg_obj = nullptr;
        _label_city = nullptr;
        _label_temp = nullptr;
        _label_condition = nullptr;
        _btn_refresh = nullptr;
        _label_refresh = nullptr;
    }
    return true;
}

void AppWeather::update_weather(const char* city, const char* temp, const char* condition) {
    _current_city = city;
    _current_temp = temp;
    _current_condition = condition;

    if (_bg_obj && _label_city && _label_temp && _label_condition) {
        lv_label_set_text(_label_city, _current_city.c_str());
        lv_label_set_text_fmt(_label_temp, "%s C", _current_temp.c_str());
        lv_label_set_text(_label_condition, _current_condition.c_str());
    }
}

void AppWeather::update_advanced_weather(const char* city, const char* temp, const char* condition, 
                                         const char* d0, const char* d1, const char* d2) {
    _current_city = city;
    _current_temp = temp;
    _current_condition = condition;
    _day0 = d0;
    _day1 = d1;
    _day2 = d2;

    if (_bg_obj) {
        close();
        run(); // re-render entirely to update cards
    }
}

extern "C" void app_weather_update_from_ble(const char* city, const char* temp, const char* condition) {
    if (bsp_display_lock(1000)) {
        if (AppWeather::get_instance()) {
            AppWeather::get_instance()->update_weather(city, temp, condition);
        }
        bsp_display_unlock();
    }
}

extern "C" void app_weather_update_advanced_from_ble(const char* city, const char* temp, const char* condition, 
                                                     const char* d0, const char* d1, const char* d2) {
    if (bsp_display_lock(1000)) {
        if (AppWeather::get_instance()) {
            AppWeather::get_instance()->update_advanced_weather(city, temp, condition, d0, d1, d2);
        }
        bsp_display_unlock();
    }
}

