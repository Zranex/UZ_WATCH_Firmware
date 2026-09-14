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
    void fetch_weather_via_wifi();
    static AppWeather* get_instance() { return _instance; }

private:
    static void on_refresh_clicked(lv_event_t* e);
    static void weather_http_task(void* pvParameters);

    lv_obj_t* _bg_obj;
    lv_obj_t* _label_city;
    lv_obj_t* _label_temp;
    lv_obj_t* _label_condition;
    lv_obj_t* _btn_refresh;
    lv_obj_t* _label_refresh;
    lv_obj_t* _label_card_icon[3] = {nullptr};
    lv_obj_t* _label_card_mm[3] = {nullptr};

    std::string _current_city = "Bekleniyor...";
    std::string _current_temp = "--";
    std::string _current_condition = "Baglanti Yok";
    
    std::string _day0 = "- / -/0";
    std::string _day1 = "- / -/0";
    std::string _day2 = "- / -/0";

    bool _is_fetching = false;

    static AppWeather* _instance;
};
