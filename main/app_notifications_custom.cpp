#include "app_notifications_custom.hpp"
#include "esp_lib_utils.h"
#include "bsp/esp-bsp.h"

// Define static history
std::vector<NotificationData> AppNotificationsCustom::history;
std::mutex AppNotificationsCustom::history_mutex;
AppNotificationsCustom* AppNotificationsCustom::_instance = nullptr;

AppNotificationsCustom::AppNotificationsCustom() : App("Notifications", nullptr, true) {
    _bg_obj = nullptr;
    list_container = nullptr;
    btn_clear = nullptr;
    _instance = this;
}

AppNotificationsCustom::~AppNotificationsCustom() {
    if (_instance == this) _instance = nullptr;
}

void AppNotificationsCustom::push_notification(const std::string& sender, const std::string& message) {
    std::lock_guard<std::mutex> lock(history_mutex);
    
    NotificationData data;
    data.sender = sender;
    data.message = message;
    data.timestamp = 0; // Or use an actual RTC timestamp if preferred

    // Add to the front of the vector
    history.insert(history.begin(), data);

    // Keep size under limit
    if (history.size() > max_history_size) {
        history.pop_back();
    }
}

void AppNotificationsCustom::update_ui_if_open() {
    if (_instance) {
        _instance->refresh_list_ui();
    }
}

std::vector<NotificationData> AppNotificationsCustom::get_history() {
    std::lock_guard<std::mutex> lock(history_mutex);
    return history;
}

void AppNotificationsCustom::clear_history() {
    std::lock_guard<std::mutex> lock(history_mutex);
    history.clear();
}

bool AppNotificationsCustom::run() {
    ESP_UTILS_LOGI("AppNotificationsCustom running");
    
    // Background
    _bg_obj = lv_obj_create(lv_scr_act());
    lv_obj_set_size(_bg_obj, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(_bg_obj, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(_bg_obj, 0, 0);
    lv_obj_set_style_radius(_bg_obj, 0, 0);

    // Title label
    lv_obj_t* title_label = lv_label_create(_bg_obj);
    lv_label_set_text(title_label, "Notifications");
    lv_obj_set_style_text_color(title_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_20, 0);
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 40);

    // List container
    list_container = lv_obj_create(_bg_obj);
    lv_obj_set_size(list_container, LV_PCT(90), LV_PCT(60));
    lv_obj_align(list_container, LV_ALIGN_CENTER, 0, 10);
    lv_obj_set_flex_flow(list_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(list_container, lv_color_hex(0x111111), 0);
    lv_obj_set_style_border_width(list_container, 0, 0);
    lv_obj_set_style_pad_all(list_container, 5, 0);

    // Populate list
    refresh_list_ui();

    // Clear Button
    btn_clear = lv_btn_create(_bg_obj);
    lv_obj_set_size(btn_clear, 120, 40);
    lv_obj_align(btn_clear, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_bg_color(btn_clear, lv_color_hex(0xCC0000), 0);
    lv_obj_add_event_cb(btn_clear, on_clear_clicked, LV_EVENT_CLICKED, this);

    lv_obj_t* btn_lbl = lv_label_create(btn_clear);
    lv_label_set_text(btn_lbl, "Clear");
    lv_obj_center(btn_lbl);

    return true;
}

bool AppNotificationsCustom::back() {
    return close();
}

bool AppNotificationsCustom::close() {
    ESP_UTILS_LOGI("AppNotificationsCustom closing");
    if (_bg_obj) {
        lv_obj_del(_bg_obj);
        _bg_obj = nullptr;
        list_container = nullptr;
    }
    return true;
}

void AppNotificationsCustom::refresh_list_ui() {
    if (!list_container) return;

    lv_obj_clean(list_container);

    std::vector<NotificationData> current_history = get_history();
    if (current_history.empty()) {
        lv_obj_t* empty_label = lv_label_create(list_container);
        lv_label_set_text(empty_label, "No notifications");
        lv_obj_set_style_text_color(empty_label, lv_color_hex(0x888888), 0);
        lv_obj_set_style_text_font(empty_label, &lv_font_montserrat_16, 0);
        lv_obj_align(empty_label, LV_ALIGN_CENTER, 0, 0);
    } else {
        for (const auto& item : current_history) {
            lv_obj_t* item_card = lv_obj_create(list_container);
            lv_obj_set_width(item_card, LV_PCT(100));
            lv_obj_set_height(item_card, LV_SIZE_CONTENT);
            lv_obj_set_style_bg_color(item_card, lv_color_hex(0x222222), 0);
            lv_obj_set_style_border_width(item_card, 0, 0);
            lv_obj_set_style_pad_all(item_card, 10, 0);

            lv_obj_t* sender_lbl = lv_label_create(item_card);
            lv_label_set_text(sender_lbl, item.sender.c_str());
            lv_obj_set_style_text_color(sender_lbl, lv_color_hex(0x25D366), 0);
            lv_obj_set_style_text_font(sender_lbl, &lv_font_montserrat_16, 0);

            lv_obj_t* msg_lbl = lv_label_create(item_card);
            lv_label_set_text(msg_lbl, item.message.c_str());
            lv_obj_set_style_text_color(msg_lbl, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_text_font(msg_lbl, &lv_font_montserrat_14, 0);
            lv_obj_set_width(msg_lbl, LV_PCT(100));
            lv_label_set_long_mode(msg_lbl, LV_LABEL_LONG_WRAP);
            
            lv_obj_align(sender_lbl, LV_ALIGN_TOP_LEFT, 0, 0);
            lv_obj_align(msg_lbl, LV_ALIGN_TOP_LEFT, 0, 20);
        }
    }
}

void AppNotificationsCustom::on_clear_clicked(lv_event_t* e) {
    AppNotificationsCustom* app = (AppNotificationsCustom*)lv_event_get_user_data(e);
    
    // Clear history
    clear_history();
    
    // Refresh UI
    if (app) {
        app->refresh_list_ui();
    }
}
