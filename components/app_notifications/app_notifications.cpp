#include <iterator>
#include <thread>
#include <unistd.h>
#include "app_notifications.hpp"

#define ESP_UTILS_LOG_TAG "App:Notifications"
#include "esp_lib_utils.h"

#define APP_NAME "Notifications"

using namespace std;
using namespace esp_brookesia::gui;
using namespace esp_brookesia::systems::phone;

namespace esp_brookesia::apps {

    std::vector<NotificationData> Notifications::history;
    std::mutex Notifications::history_mutex;

    const std::vector<NotificationData>& Notifications::get_history() {
        return history;
    }

    void Notifications::clear_history() {
        std::lock_guard<std::mutex> lock(history_mutex);
        history.clear();
    }

    std::mutex& Notifications::get_history_mutex() {
        return history_mutex;
    }

    void Notifications::push_notification(const std::string& sender, const std::string& message) {
        std::lock_guard<std::mutex> lock(history_mutex);
        if (history.size() >= MAX_HISTORY_SIZE) {
            history.erase(history.begin());
        }
        // Insert at the beginning so the newest is first
        history.insert(history.begin(), {sender, message});
    }

    Notifications::Notifications() :
        App({
        .name = APP_NAME,
        .launcher_icon = gui::StyleImage::IMAGE(&app_notifications_112_112),
        .screen_size = gui::StyleSize::RECT_PERCENT(100, 100),
        .flags = {
            .enable_default_screen = 1,
            .enable_recycle_resource = 0,
            .enable_resize_visual_area = 1,
        },
        },
        {
        .app_launcher_page_index = 0,
        .flags = {
            .enable_navigation_gesture = 1,
        },
        })
    {}

    Notifications::~Notifications()
    {
        ESP_UTILS_LOGD("Destroy(@0x%p)", this);
    }


    bool Notifications::run()
    {
        ESP_UTILS_LOGD("Run");

        lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x000000), 0);

        // Header
        lv_obj_t* header = lv_obj_create(lv_scr_act());
        lv_obj_set_size(header, LV_PCT(100), 60);
        lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_set_style_bg_color(header, lv_color_hex(0x111111), 0);
        lv_obj_set_style_border_width(header, 0, 0);
        lv_obj_set_style_radius(header, 0, 0);

        lv_obj_t* title = lv_label_create(header);
        lv_obj_set_style_text_font(title, &esp_brookesia_font_maison_neue_book_24, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
        lv_label_set_text(title, "Notifications");
        lv_obj_align(title, LV_ALIGN_LEFT_MID, 10, 0);

        // Clear Button
        clear_btn = lv_btn_create(header);
        lv_obj_align(clear_btn, LV_ALIGN_RIGHT_MID, -10, 0);
        lv_obj_set_style_bg_color(clear_btn, lv_color_hex(0xFF3333), 0);
        lv_obj_t* clear_label = lv_label_create(clear_btn);
        lv_label_set_text(clear_label, "Clear");
        lv_obj_add_event_cb(clear_btn, clear_btn_event_cb, LV_EVENT_CLICKED, this);

        // List Container
        list_container = lv_obj_create(lv_scr_act());
        lv_obj_set_size(list_container, LV_PCT(100), 400); // Remaining height
        lv_obj_align(list_container, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_color(list_container, lv_color_hex(0x000000), 0);
        lv_obj_set_style_border_width(list_container, 0, 0);
        lv_obj_set_style_pad_all(list_container, 10, 0);
        lv_obj_set_flex_flow(list_container, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(list_container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        
        empty_label = lv_label_create(list_container);
        lv_label_set_text(empty_label, "No notifications");
        lv_obj_set_style_text_color(empty_label, lv_color_hex(0x888888), 0);
        lv_obj_add_flag(empty_label, LV_OBJ_FLAG_HIDDEN);

        refresh_list();

        return true;
    }

    void Notifications::refresh_list() {
        if (!list_container) return;

        // Clear all children except empty_label
        uint32_t child_cnt = lv_obj_get_child_cnt(list_container);
        for (int i = child_cnt - 1; i >= 0; i--) {
            lv_obj_t* child = lv_obj_get_child(list_container, i);
            if (child != empty_label) {
                lv_obj_del(child);
            }
        }

        std::lock_guard<std::mutex> lock(history_mutex);

        if (history.empty()) {
            lv_obj_clear_flag(empty_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(empty_label, LV_OBJ_FLAG_HIDDEN);

            for (const auto& notif : history) {
                lv_obj_t* item = lv_obj_create(list_container);
                lv_obj_set_width(item, LV_PCT(100));
                // lv_obj_set_height(item, LV_SIZE_CONTENT); // Auto height
                lv_obj_set_style_bg_color(item, lv_color_hex(0x1A1A1A), 0);
                lv_obj_set_style_border_width(item, 0, 0);
                lv_obj_set_style_radius(item, 15, 0);
                lv_obj_set_style_pad_all(item, 10, 0);
                lv_obj_set_flex_flow(item, LV_FLEX_FLOW_COLUMN);

                lv_obj_t* sender_label = lv_label_create(item);
                lv_label_set_text(sender_label, notif.sender.c_str());
                lv_obj_set_style_text_color(sender_label, lv_color_hex(0x25D366), 0); // WhatsApp Green
                lv_obj_set_style_text_font(sender_label, &esp_brookesia_font_maison_neue_book_24, 0);

                lv_obj_t* msg_label = lv_label_create(item);
                lv_label_set_text(msg_label, notif.message.c_str());
                lv_obj_set_style_text_color(msg_label, lv_color_hex(0xFFFFFF), 0);
                lv_label_set_long_mode(msg_label, LV_LABEL_LONG_WRAP);
                lv_obj_set_width(msg_label, LV_PCT(100));
            }
        }
    }

    void Notifications::clear_btn_event_cb(lv_event_t* e) {
        Notifications* app = (Notifications*)lv_event_get_user_data(e);
        app->clear_history();
        app->refresh_list();
    }

    bool Notifications::back()
    {
        ESP_UTILS_LOGD("Back");
        ESP_UTILS_CHECK_FALSE_RETURN(notifyCoreClosed(), false, "Notify core closed failed");
        return true;
    }

    bool Notifications::close()
    {
        ESP_UTILS_LOGD("Close");

        return true;
    }

    bool Notifications::init()
    {
        ESP_UTILS_LOGD("Init");

        return true;
    }

    bool Notifications::deinit()
    {
        ESP_UTILS_LOGD("Deinit");

        return true;
    }


    ESP_UTILS_REGISTER_PLUGIN(systems::base::App, Notifications, APP_NAME)

}