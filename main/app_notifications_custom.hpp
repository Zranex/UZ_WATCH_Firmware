#pragma once

#include "esp_brookesia.hpp"
#include <string>
#include <vector>
#include <mutex>

struct NotificationData {
    std::string sender;
    std::string message;
    uint32_t timestamp;
};

class AppNotificationsCustom : public esp_brookesia::systems::phone::App {
public:
    AppNotificationsCustom();
    ~AppNotificationsCustom();

    // Static history management methods
    static void push_notification(const std::string& sender, const std::string& message);
    static std::vector<NotificationData> get_history();
    static void clear_history();
    static void update_ui_if_open();

protected:
    bool run() override;
    bool back() override;
    bool close() override;

private:
    static void on_clear_clicked(lv_event_t* e);
    
    lv_obj_t* _bg_obj = nullptr;
    lv_obj_t* list_container = nullptr;
    lv_obj_t* btn_clear = nullptr;

    // History state
    static std::vector<NotificationData> history;
    static std::mutex history_mutex;
    static const size_t max_history_size = 30;

    static AppNotificationsCustom* _instance;
    void refresh_list_ui();
};
