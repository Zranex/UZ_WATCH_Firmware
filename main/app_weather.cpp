#include "app_weather.hpp"
#include "esp_log.h"
#include <stdio.h>
extern const lv_image_dsc_t icon_weather;

AppWeather* AppWeather::_instance = nullptr;

AppWeather::AppWeather() : esp_brookesia::systems::phone::App("Hava Durumu", &icon_weather, true) {
    _bg_obj = nullptr;
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

bool AppWeather::run() {
    if (_bg_obj != nullptr) return true;

    _bg_obj = lv_obj_create(lv_scr_act());
    lv_obj_set_size(_bg_obj, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(_bg_obj, lv_color_hex(0x202124), 0); // Koyu gri (Google havasi)
    lv_obj_set_style_border_width(_bg_obj, 0, 0);
    lv_obj_set_style_radius(_bg_obj, 0, 0);
    lv_obj_clear_flag(_bg_obj, LV_OBJ_FLAG_SCROLLABLE);

    // City Label
    _label_city = lv_label_create(_bg_obj);
    lv_obj_set_style_text_font(_label_city, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(_label_city, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(_label_city, _current_city.c_str());
    lv_obj_align(_label_city, LV_ALIGN_TOP_MID, 0, 60);

    // Temp Label
    _label_temp = lv_label_create(_bg_obj);
    lv_obj_set_style_text_font(_label_temp, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(_label_temp, lv_color_hex(0xFABD04), 0); // Google Gunes Sarisi
    lv_label_set_text_fmt(_label_temp, "%s C", _current_temp.c_str());
    lv_obj_align(_label_temp, LV_ALIGN_TOP_MID, 0, 110);

    // Condition Label
    _label_condition = lv_label_create(_bg_obj);
    lv_obj_set_style_text_font(_label_condition, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(_label_condition, lv_color_hex(0x9AA0A6), 0);
    lv_label_set_text(_label_condition, _current_condition.c_str());
    lv_obj_align(_label_condition, LV_ALIGN_TOP_MID, 0, 170);

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

    return true;
}

bool AppWeather::back() {
    return close();
}

bool AppWeather::close() {
    if (_bg_obj) {
        lv_obj_del(_bg_obj);
        _bg_obj = nullptr;
    }
    return true;
}

void AppWeather::update_weather(const char* city, const char* temp, const char* condition) {
    _current_city = city;
    _current_temp = temp;
    _current_condition = condition;

    if (_bg_obj) {
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
    if (AppWeather::get_instance()) {
        AppWeather::get_instance()->update_weather(city, temp, condition);
    }
}

extern "C" void app_weather_update_advanced_from_ble(const char* city, const char* temp, const char* condition, 
                                                     const char* d0, const char* d1, const char* d2) {
    if (AppWeather::get_instance()) {
        AppWeather::get_instance()->update_advanced_weather(city, temp, condition, d0, d1, d2);
    }
}
