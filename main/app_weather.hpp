#pragma once
#include "esp_brookesia.hpp"
#include "lvgl.h"
#include <string>

class AppWeather : public esp_brookesia::systems::phone::App {
public:
    AppWeather();
    ~AppWeather();

    bool run() override;
    bool back() override;
    bool close() override;

    void update_weather(const char* city, const char* temp, const char* condition);
    void update_advanced_weather(const char* city, const char* temp, const char* condition, 
                                 const char* d0, const char* d1, const char* d2);
    static AppWeather* get_instance() { return _instance; }

private:
    lv_obj_t* _bg_obj;
    lv_obj_t* _label_city;
    lv_obj_t* _label_temp;
    lv_obj_t* _label_condition;

    std::string _current_city = "Bekleniyor...";
    std::string _current_temp = "--";
    std::string _current_condition = "Baglanti Yok";
    
    std::string _day0 = "- / -/0";
    std::string _day1 = "- / -/0";
    std::string _day2 = "- / -/0";

    static AppWeather* _instance;
};
