#pragma once

#include "esp_brookesia.hpp"
#include "lvgl.h"
#include <string>

class AppTransit : public esp_brookesia::systems::phone::App {
public:
    AppTransit();
    virtual ~AppTransit();

    virtual bool run() override;
    virtual bool back() override;
    virtual bool close() override;
    virtual bool init() override;

    void update_route(int index, const char* time_str, const char* sub_str);
    void request_refresh();
    void compute_offline_estimates();
    void refresh_ui_if_visible();
    void request_close() { notifyCoreClosed(); }
    static void force_close();
    static AppTransit* get_instance() { return _instance; }

    void show_card_modal();
    void hide_card_modal();

private:
    static void on_refresh_clicked(lv_event_t* e);
    static void on_card_clicked(lv_event_t* e);

    lv_obj_t* _bg_obj;
    lv_obj_t* _btn_back;
    lv_obj_t* _lbl_title;
    lv_obj_t* _lbl_status;
    lv_obj_t* _btn_refresh;
    lv_obj_t* _lbl_refresh;
    lv_obj_t* _btn_card;
    lv_obj_t* _modal_card;

    uint8_t _saved_brightness;
    std::string _istanbulkart_id;
    int _current_card_index;
    lv_obj_t* _card_obj;
    lv_obj_t* _lbl_card_title;
    lv_obj_t* _lbl_card_sub;
    lv_obj_t* _lbl_card_id;
    lv_obj_t* _qr_code;
    void update_active_card();

    struct RouteCardUI {
        const char* badge_name;
        lv_color_t badge_color;
        const char* route_name;
        const char* direction;
        std::string current_time;
        std::string current_sub;
        lv_obj_t* card_obj;
        lv_obj_t* lbl_time;
        lv_obj_t* lbl_sub;
    };

    RouteCardUI _routes[4];
    bool _is_fetching;
    static AppTransit* _instance;
};

extern "C" void app_transit_update_from_ble(const char* payload);
extern "C" void app_transit_request_refresh_from_watch(void);
