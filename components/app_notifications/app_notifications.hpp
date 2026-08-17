#pragma once

#include "lvgl.h"
#include "esp_brookesia.hpp"
#include "assets/app_notifications_assets.h"
#include <vector>
#include <string>
#include <mutex>

namespace esp_brookesia::apps {

    struct NotificationData {
        std::string sender;
        std::string message;
    };

    class Notifications : public systems::phone::App {
    public:
        Notifications();
        ~Notifications();

        bool run() override;
        bool back() override;
        bool close() override;
        bool init() override;
        bool deinit() override;

        // History methods
        static const std::vector<NotificationData>& get_history();
        static void clear_history();
        static std::mutex& get_history_mutex();
        static void push_notification(const std::string& sender, const std::string& message);

    private:
        static std::vector<NotificationData> history;
        static std::mutex history_mutex;
        static const size_t MAX_HISTORY_SIZE = 30;

        lv_obj_t* list_container;
        lv_obj_t* empty_label;
        lv_obj_t* clear_btn;
        
        void refresh_list();
        static void clear_btn_event_cb(lv_event_t* e);
    };

}
