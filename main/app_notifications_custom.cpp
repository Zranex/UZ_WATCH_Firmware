#include "app_notifications_custom.hpp"
#include "ui_notification_banner.hpp"
extern const lv_image_dsc_t icon_notifications;
#include "esp_lib_utils.h"
#include "bsp/esp-bsp.h"
#include "bsp_board_extra.h"

extern "C" {
#include "ble_manager.h"
}

// Define static history
std::vector<NotificationData> AppNotificationsCustom::history;
std::mutex AppNotificationsCustom::history_mutex;
AppNotificationsCustom* AppNotificationsCustom::_instance = nullptr;

AppNotificationsCustom::AppNotificationsCustom() : App("Bildirimler", &icon_notifications, true) {
    _bg_obj = nullptr;
    list_container = nullptr;
    btn_clear = nullptr;
    _instance = this;
}

AppNotificationsCustom::~AppNotificationsCustom() {
    if (_instance == this) _instance = nullptr;
}

void AppNotificationsCustom::push_notification(const std::string& sender, const std::string& message, const std::string& notif_id) {
    std::lock_guard<std::mutex> lock(history_mutex);

    NotificationData data;
    data.sender = sender;
    data.message = message;
    data.notif_id = notif_id;
    data.timestamp = 0;

    history.insert(history.begin(), data);

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
    if (_bg_obj != nullptr) return true;
    ESP_UTILS_LOGI("AppNotificationsCustom running");

    _bg_obj = lv_obj_create(lv_scr_act());
    lv_obj_set_size(_bg_obj, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(_bg_obj, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(_bg_obj, 0, 0);
    lv_obj_set_style_radius(_bg_obj, 0, 0);

    lv_obj_t* title_label = lv_label_create(_bg_obj);
    lv_label_set_text(title_label, "Bildirimler");
    lv_obj_set_style_text_color(title_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_20, 0);
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 36);

    list_container = lv_obj_create(_bg_obj);
    lv_obj_set_size(list_container, LV_PCT(92), LV_PCT(62));
    lv_obj_align(list_container, LV_ALIGN_CENTER, 0, 8);
    lv_obj_set_flex_flow(list_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(list_container, lv_color_hex(0x111111), 0);
    lv_obj_set_style_radius(list_container, 18, 0);
    lv_obj_set_style_border_width(list_container, 0, 0);
    lv_obj_set_style_pad_all(list_container, 8, 0);
    lv_obj_set_style_pad_row(list_container, 8, 0);

    refresh_list_ui();

    btn_clear = lv_btn_create(_bg_obj);
    lv_obj_set_style_shadow_width(btn_clear, 0, 0);
    lv_obj_set_style_shadow_spread(btn_clear, 0, 0);
    lv_obj_set_style_shadow_opa(btn_clear, LV_OPA_TRANSP, 0);
    lv_obj_set_size(btn_clear, 140, 42);
    lv_obj_align(btn_clear, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_radius(btn_clear, 21, 0);
    lv_obj_set_style_bg_color(btn_clear, lv_color_hex(0xCC0000), 0);
    lv_obj_add_event_cb(btn_clear, on_clear_clicked, LV_EVENT_CLICKED, this);

    lv_obj_t* btn_lbl = lv_label_create(btn_clear);
    lv_label_set_text(btn_lbl, "Temizle");
    lv_obj_center(btn_lbl);

    return true;
}

bool AppNotificationsCustom::back() {
    return notifyCoreClosed();
}

bool AppNotificationsCustom::close() {
    ESP_UTILS_LOGI("AppNotificationsCustom closing");
    if (_bg_obj) {
        lv_obj_delete_async(_bg_obj);
        _bg_obj = nullptr;
        list_container = nullptr;
        btn_clear = nullptr;
    }
    return true;
}

void AppNotificationsCustom::refresh_list_ui() {
    if (!list_container) return;

    lv_obj_clean(list_container);

    std::vector<NotificationData> current_history = get_history();
    if (current_history.empty()) {
        lv_obj_t* empty_label = lv_label_create(list_container);
        lv_label_set_text(empty_label, "Henuz bildirim yok");
        lv_obj_set_style_text_color(empty_label, lv_color_hex(0x888888), 0);
        lv_obj_set_style_text_font(empty_label, &lv_font_montserrat_16, 0);
        lv_obj_align(empty_label, LV_ALIGN_CENTER, 0, 0);
    } else {
        for (const auto& item : current_history) {
            lv_obj_t* item_card = lv_obj_create(list_container);
            lv_obj_set_width(item_card, LV_PCT(100));
            lv_obj_set_height(item_card, LV_SIZE_CONTENT);
            lv_obj_set_style_radius(item_card, 14, 0);
            lv_obj_set_style_bg_color(item_card, lv_color_hex(0x1E1E24), 0);
            lv_obj_set_style_border_width(item_card, 0, 0);
            lv_obj_set_style_shadow_width(item_card, 0, 0);
            lv_obj_set_style_pad_all(item_card, 12, 0);

            lv_obj_t* sender_lbl = lv_label_create(item_card);
            lv_label_set_text(sender_lbl, item.sender.c_str());
            lv_obj_set_style_text_color(sender_lbl, lv_color_hex(0x25D366), 0);
            lv_obj_set_style_text_font(sender_lbl, &lv_font_montserrat_16, 0);
            lv_obj_set_width(sender_lbl, !item.notif_id.empty() ? 200 : 300);
            lv_label_set_long_mode(sender_lbl, LV_LABEL_LONG_DOT);

            lv_obj_t* msg_lbl = lv_label_create(item_card);
            lv_label_set_text(msg_lbl, item.message.c_str());
            lv_obj_set_style_text_color(msg_lbl, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_text_font(msg_lbl, &lv_font_montserrat_14, 0);
            lv_obj_set_width(msg_lbl, LV_PCT(100));
            lv_label_set_long_mode(msg_lbl, LV_LABEL_LONG_WRAP);

            lv_obj_align(sender_lbl, LV_ALIGN_TOP_LEFT, 0, 0);
            lv_obj_align(msg_lbl, LV_ALIGN_TOP_LEFT, 0, 26);

            if (!item.notif_id.empty()) {
                lv_obj_t* reply_btn = lv_btn_create(item_card);
                lv_obj_set_style_shadow_width(reply_btn, 0, 0);
                lv_obj_set_style_shadow_spread(reply_btn, 0, 0);
                lv_obj_set_style_shadow_opa(reply_btn, LV_OPA_TRANSP, 0);
                lv_obj_set_size(reply_btn, 92, 32);
                lv_obj_set_style_radius(reply_btn, 10, 0);
                lv_obj_align(reply_btn, LV_ALIGN_TOP_RIGHT, 0, -2);
                lv_obj_set_style_bg_color(reply_btn, lv_color_hex(0x0055FF), 0);

                char* id_copy = strdup(item.notif_id.c_str());
                lv_obj_add_event_cb(reply_btn, on_reply_clicked, LV_EVENT_CLICKED, id_copy);
                lv_obj_add_event_cb(reply_btn, on_user_data_deleted, LV_EVENT_DELETE, id_copy);

                lv_obj_t* btn_lbl = lv_label_create(reply_btn);
                lv_label_set_text(btn_lbl, "Yanitla");
                lv_obj_set_style_text_font(btn_lbl, &lv_font_montserrat_14, 0);
                lv_obj_center(btn_lbl);

                lv_obj_set_style_pad_bottom(item_card, 16, 0);
            }
        }
    }
}

void AppNotificationsCustom::on_user_data_deleted(lv_event_t* e) {
    char* id_copy = (char*)lv_event_get_user_data(e);
    if (id_copy) {
        free(id_copy);
    }
}

void AppNotificationsCustom::on_reply_clicked(lv_event_t* e) {
    char* notif_id = (char*)lv_event_get_user_data(e);
    if (!notif_id) return;

    lv_obj_t* mbox = lv_obj_create(lv_scr_act());
    lv_obj_set_size(mbox, LV_PCT(90), LV_PCT(82));
    lv_obj_center(mbox);
    lv_obj_set_style_bg_color(mbox, lv_color_hex(0x141418), 0);
    lv_obj_set_style_radius(mbox, 22, 0);
    lv_obj_set_style_border_width(mbox, 2, 0);
    lv_obj_set_style_border_color(mbox, lv_color_hex(0x33333D), 0);
    lv_obj_set_style_shadow_width(mbox, 0, 0);
    lv_obj_set_flex_flow(mbox, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(mbox, 8, 0);

    lv_obj_t* title = lv_label_create(mbox);
    lv_label_set_text(title, "Hizli Yanit Sec:");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);

    for (int i = 0; i <= AppNotifications::QUICK_REPLY_COUNT; i++) {
        bool is_cancel = (i == AppNotifications::QUICK_REPLY_COUNT);
        std::string text = is_cancel ? "Iptal" : AppNotifications::get_quick_reply(i);

        lv_obj_t* btn = lv_btn_create(mbox);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_shadow_spread(btn, 0, 0);
        lv_obj_set_style_shadow_opa(btn, LV_OPA_TRANSP, 0);
        lv_obj_set_width(btn, LV_PCT(100));
        lv_obj_set_height(btn, 44);
        lv_obj_set_style_radius(btn, 14, 0);
        lv_obj_set_style_bg_color(btn, is_cancel ? lv_color_hex(0x4A1C1C) : lv_color_hex(0x282832), 0);

        char* payload = (char*)malloc(256);
        if (!payload) continue;
        snprintf(payload, 256, "%s|%s", notif_id, text.c_str());
        lv_obj_add_event_cb(btn, on_reply_selected, LV_EVENT_CLICKED, payload);
        lv_obj_add_event_cb(btn, on_user_data_deleted, LV_EVENT_DELETE, payload);

        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, text.c_str());
        lv_obj_set_width(lbl, LV_PCT(92));
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_center(lbl);
    }
}

void AppNotificationsCustom::on_reply_selected(lv_event_t* e) {
    char* payload = (char*)lv_event_get_user_data(e);
    lv_obj_t* btn = (lv_obj_t*)lv_event_get_target(e);
    lv_obj_t* mbox = lv_obj_get_parent(btn);

    if (payload) {
        char* separator = strchr(payload, '|');
        if (separator) {
            *separator = '\0';
            const char* notif_id = payload;
            const char* reply_text = separator + 1;

            if (strcmp(reply_text, "Iptal") != 0) {
                char ble_cmd[256];
                snprintf(ble_cmd, sizeof(ble_cmd), "REPLY|%s|%s", notif_id, reply_text);
                ESP_LOGI("Notifications", "Sending BLE Quick Reply: %s", ble_cmd);
                ble_manager_send_media_command(ble_cmd);
                bsp_vibrate_pulse(50);
            }
        }
    }

    lv_obj_delete_async(mbox);
}

void AppNotificationsCustom::on_clear_clicked(lv_event_t* e) {
    AppNotificationsCustom* app = (AppNotificationsCustom*)lv_event_get_user_data(e);
    clear_history();
    if (app) {
        app->refresh_list_ui();
    }
}
