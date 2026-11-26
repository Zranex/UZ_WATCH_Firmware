#include "ui_notification_banner.hpp"
#include "esp_log.h"
#include <string.h>
#include <mutex>
#include "nvs_flash.h"
#include "nvs.h"
#include "app_notifications_custom.hpp"
#include "bsp/esp-bsp.h"
#include "bsp_board_extra.h"
#include "display_manager.h"
#include "app_power.hpp"
#include "app_school.hpp"

extern "C" {
#include "ble_manager.h"
}

static const char *TAG = "app_notifications";

static std::mutex s_qr_mutex;
static std::string s_quick_replies[AppNotifications::QUICK_REPLY_COUNT] = {
    "Dersteyim, cikinca arayacagim.",
    "Geliyorum",
    "Tamam",
    "5 dk'ya oradayim"
};

lv_obj_t* AppNotifications::container = nullptr;
lv_obj_t* AppNotifications::title_label = nullptr;
lv_obj_t* AppNotifications::message_label = nullptr;
lv_obj_t* AppNotifications::reply_btn = nullptr;
lv_timer_t* AppNotifications::hide_timer = nullptr;
std::string AppNotifications::current_banner_notif_id = "";

lv_obj_t* AppNotifications::nav_container = nullptr;
lv_obj_t* AppNotifications::nav_icon_box = nullptr;
lv_obj_t* AppNotifications::nav_icon_label = nullptr;
lv_obj_t* AppNotifications::nav_dist_label = nullptr;
lv_obj_t* AppNotifications::nav_street_label = nullptr;
lv_timer_t* AppNotifications::nav_timeout_timer = nullptr;
char AppNotifications::last_nav_dir = '\0';
std::string AppNotifications::last_nav_street = "";
bool AppNotifications::nav_user_dismissed_current_step = false;

void AppNotifications::load_quick_replies_from_nvs() {
    std::lock_guard<std::mutex> lock(s_qr_mutex);
    nvs_handle_t nvs_h;
    if (nvs_open("settings", NVS_READONLY, &nvs_h) == ESP_OK) {
        for (int i = 0; i < QUICK_REPLY_COUNT; i++) {
            char key[16];
            snprintf(key, sizeof(key), "qr_%d", i);
            char val[64] = {0};
            size_t len = sizeof(val);
            if (nvs_get_str(nvs_h, key, val, &len) == ESP_OK && strlen(val) > 0) {
                s_quick_replies[i] = val;
            }
        }
        nvs_close(nvs_h);
    }
}

void AppNotifications::update_quick_replies(const char* payload) {
    if (!payload || strlen(payload) == 0) return;

    char buf[256];
    strncpy(buf, payload, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    std::lock_guard<std::mutex> lock(s_qr_mutex);
    char* saveptr = nullptr;
    char* token = strtok_r(buf, "|", &saveptr);
    int idx = 0;
    bool any_changed = false;
    std::string incoming[QUICK_REPLY_COUNT];
    bool has_val[QUICK_REPLY_COUNT] = {false};

    while (token != nullptr && idx < QUICK_REPLY_COUNT) {
        if (strlen(token) > 0) {
            incoming[idx] = token;
            has_val[idx] = true;
            if (s_quick_replies[idx] != incoming[idx]) {
                any_changed = true;
            }
        }
        idx++;
        token = strtok_r(nullptr, "|", &saveptr);
    }

    if (!any_changed) {
        return; // Identical quick replies -> skip NVS commit & dual-core SPI flash stall!
    }

    nvs_handle_t nvs_h;
    bool nvs_ok = (nvs_open("settings", NVS_READWRITE, &nvs_h) == ESP_OK);
    for (int i = 0; i < QUICK_REPLY_COUNT; i++) {
        if (has_val[i]) {
            s_quick_replies[i] = incoming[i];
            if (nvs_ok) {
                char key[16];
                snprintf(key, sizeof(key), "qr_%d", i);
                nvs_set_str(nvs_h, key, incoming[i].c_str());
            }
            ESP_LOGI(TAG, "Updated Quick Reply [%d]: %s", i, incoming[i].c_str());
        }
    }

    if (nvs_ok) {
        nvs_commit(nvs_h);
        nvs_close(nvs_h);
    }
}

std::string AppNotifications::get_quick_reply(int index) {
    std::lock_guard<std::mutex> lock(s_qr_mutex);
    if (index >= 0 && index < QUICK_REPLY_COUNT) {
        return s_quick_replies[index];
    }
    return "Tamam";
}

void AppNotifications::init() {
    load_quick_replies_from_nvs();

    // 1. Live Google Maps Navigation Overlay (sits cleanly at top when active)
    nav_container = lv_obj_create(lv_layer_top());
    lv_obj_set_size(nav_container, 360, 86);
    lv_obj_align(nav_container, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_set_style_radius(nav_container, 20, 0);
    lv_obj_set_style_bg_color(nav_container, lv_color_hex(0x0B1E13), 0);
    lv_obj_set_style_border_width(nav_container, 2, 0);
    lv_obj_set_style_border_color(nav_container, lv_color_hex(0x00E676), 0);
    lv_obj_set_style_pad_all(nav_container, 10, 0);
    lv_obj_set_style_shadow_width(nav_container, 0, 0);
    lv_obj_clear_flag(nav_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(nav_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(nav_container, nav_container_event_cb, LV_EVENT_CLICKED, NULL);

    nav_icon_box = lv_obj_create(nav_container);
    lv_obj_set_size(nav_icon_box, 58, 58);
    lv_obj_align(nav_icon_box, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(nav_icon_box, 14, 0);
    lv_obj_set_style_bg_color(nav_icon_box, lv_color_hex(0x0F9D58), 0);
    lv_obj_set_style_border_width(nav_icon_box, 0, 0);
    lv_obj_set_style_shadow_width(nav_icon_box, 0, 0);
    lv_obj_clear_flag(nav_icon_box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    nav_icon_label = lv_label_create(nav_icon_box);
    lv_label_set_text(nav_icon_label, LV_SYMBOL_UP);
    lv_obj_set_style_text_color(nav_icon_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(nav_icon_label, &lv_font_montserrat_28, 0);
    lv_obj_center(nav_icon_label);

    nav_dist_label = lv_label_create(nav_container);
    lv_obj_set_width(nav_dist_label, 265);
    lv_obj_align(nav_dist_label, LV_ALIGN_TOP_LEFT, 70, 4);
    lv_obj_set_style_text_color(nav_dist_label, lv_color_hex(0x00E676), 0);
    lv_obj_set_style_text_font(nav_dist_label, &lv_font_montserrat_22, 0);
    lv_label_set_long_mode(nav_dist_label, LV_LABEL_LONG_DOT);
    lv_label_set_text(nav_dist_label, "0 m");

    nav_street_label = lv_label_create(nav_container);
    lv_obj_set_width(nav_street_label, 265);
    lv_obj_align(nav_street_label, LV_ALIGN_BOTTOM_LEFT, 70, -4);
    lv_obj_set_style_text_color(nav_street_label, lv_color_hex(0xE0E0E0), 0);
    lv_obj_set_style_text_font(nav_street_label, &lv_font_montserrat_16, 0);
    lv_label_set_long_mode(nav_street_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(nav_street_label, "Rota hazir");

    nav_timeout_timer = lv_timer_create(nav_timeout_timer_cb, 180000, NULL);
    lv_timer_pause(nav_timeout_timer);

    // 2. Standard Notification Banner (created after nav_container so incoming chats sit on top)
    container = lv_obj_create(lv_layer_top());
    lv_obj_set_size(container, 340, 106);
    lv_obj_align(container, LV_ALIGN_TOP_MID, 0, -120); // Start hidden above the screen
    lv_obj_set_style_radius(container, 20, 0);
    lv_obj_set_style_bg_color(container, lv_color_hex(0x1a1a1a), 0);
    lv_obj_set_style_border_width(container, 2, 0);
    lv_obj_set_style_border_color(container, lv_color_hex(0x333333), 0);
    lv_obj_set_style_pad_all(container, 14, 0);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_shadow_width(container, 0, 0);

    // Title label (Sender)
    title_label = lv_label_create(container);
    lv_obj_set_width(title_label, LV_PCT(100));
    lv_obj_set_style_text_color(title_label, lv_color_hex(0x25D366), 0);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_20, 0);
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_DOT);
    lv_label_set_text(title_label, "Sender");

    // Message label
    message_label = lv_label_create(container);
    lv_obj_set_width(message_label, LV_PCT(100));
    lv_obj_set_style_text_color(message_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(message_label, &lv_font_montserrat_16, 0);
    lv_label_set_long_mode(message_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(message_label, "Message text goes here...");

    // Click event to dismiss
    lv_obj_add_event_cb(container, container_event_cb, LV_EVENT_CLICKED, NULL);

    // Reply Button
    reply_btn = lv_btn_create(container);
    lv_obj_set_style_shadow_width(reply_btn, 0, 0);
    lv_obj_set_style_shadow_spread(reply_btn, 0, 0);
    lv_obj_set_style_shadow_opa(reply_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_size(reply_btn, 86, 30);
    lv_obj_align(reply_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(reply_btn, lv_color_hex(0x0055FF), 0);
    lv_obj_add_flag(reply_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(reply_btn, on_banner_reply_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t* btn_lbl = lv_label_create(reply_btn);
    lv_label_set_text(btn_lbl, "Yanitla");
    lv_obj_set_style_text_font(btn_lbl, &lv_font_montserrat_12, 0);
    lv_obj_center(btn_lbl);

    // Create a timer but pause it
    hide_timer = lv_timer_create(hide_timer_cb, 5000, NULL);
    lv_timer_pause(hide_timer);
}

bool AppNotifications::show_nav(char dir_code, const char* dist_text, const char* street_text) {
    if (!nav_container) return false;

    const char* safe_street = (street_text && strlen(street_text) > 0) ? street_text : "Rotada ilerleyin";
    bool maneuver_changed = (dir_code != last_nav_dir) || (last_nav_street != safe_street);
    if (maneuver_changed) {
        last_nav_dir = dir_code;
        last_nav_street = safe_street;
        nav_user_dismissed_current_step = false;
    }

    const char* action_word = "Duz devam";
    const char* symbol = LV_SYMBOL_UP;
    lv_color_t badge_color = lv_color_hex(0x0F9D58);

    switch (dir_code) {
        case 'L':
            symbol = LV_SYMBOL_LEFT;
            action_word = "Sola don";
            break;
        case 'R':
            symbol = LV_SYMBOL_RIGHT;
            action_word = "Saga don";
            break;
        case 'U':
            symbol = LV_SYMBOL_LOOP;
            action_word = "U donusu";
            badge_color = lv_color_hex(0xFB8C00);
            break;
        case 'F':
            symbol = LV_SYMBOL_OK;
            action_word = "Varis noktasi";
            badge_color = lv_color_hex(0x1E88E5);
            break;
        default:
            symbol = LV_SYMBOL_UP;
            action_word = "Duz ilerle";
            break;
    }

    if (nav_icon_box) lv_obj_set_style_bg_color(nav_icon_box, badge_color, 0);
    if (nav_icon_label) lv_label_set_text(nav_icon_label, symbol);

    if (nav_dist_label) {
        if (dist_text && strlen(dist_text) > 0) {
            lv_label_set_text_fmt(nav_dist_label, "%s  -  %s", dist_text, action_word);
        } else {
            lv_label_set_text(nav_dist_label, action_word);
        }
    }

    if (nav_street_label) {
        lv_label_set_text(nav_street_label, safe_street);
    }

    if (!nav_user_dismissed_current_step) {
        lv_obj_clear_flag(nav_container, LV_OBJ_FLAG_HIDDEN);
    }

    if (nav_timeout_timer) {
        lv_timer_reset(nav_timeout_timer);
        lv_timer_resume(nav_timeout_timer);
    }

    return maneuver_changed;
}

void AppNotifications::hide_nav() {
    if (nav_container) {
        lv_obj_add_flag(nav_container, LV_OBJ_FLAG_HIDDEN);
    }
    if (nav_timeout_timer) {
        lv_timer_pause(nav_timeout_timer);
    }
    last_nav_dir = '\0';
    last_nav_street.clear();
    nav_user_dismissed_current_step = false;
}

void AppNotifications::nav_container_event_cb(lv_event_t* e) {
    // User tapped navigation banner to hide it for the current street/step;
    // it will automatically reappear when the next turn arrives!
    nav_user_dismissed_current_step = true;
    if (nav_container) {
        lv_obj_add_flag(nav_container, LV_OBJ_FLAG_HIDDEN);
    }
}

void AppNotifications::nav_timeout_timer_cb(lv_timer_t* timer) {
    hide_nav();
}

void AppNotifications::show(const char* sender, const char* message, const std::string& notif_id) {
    if (!container) return;

    current_banner_notif_id = notif_id;
    if (title_label) lv_label_set_text(title_label, sender ? sender : "");
    if (message_label) lv_label_set_text(message_label, message ? message : "");

    if (reply_btn) {
        if (!notif_id.empty()) {
            lv_obj_clear_flag(reply_btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(reply_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Slide down animation
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, container);
    lv_anim_set_values(&a, -110, 10);
    lv_anim_set_time(&a, 300);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_path_cb(&a, lv_anim_path_overshoot);
    lv_anim_start(&a);

    lv_timer_reset(hide_timer);
    lv_timer_resume(hide_timer);
}

void AppNotifications::hide() {
    if (!container) return;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, container);
    lv_anim_set_values(&a, lv_obj_get_y(container), -120);
    lv_anim_set_time(&a, 300);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_start(&a);

    lv_timer_pause(hide_timer);
}

void AppNotifications::hide_timer_cb(lv_timer_t* timer) {
    hide();
}

void AppNotifications::container_event_cb(lv_event_t* e) {
    hide();
}

void AppNotifications::on_banner_user_data_deleted(lv_event_t* e) {
    char* data = (char*)lv_event_get_user_data(e);
    if (data) free(data);
}

void AppNotifications::on_banner_reply_clicked(lv_event_t* e) {
    if (current_banner_notif_id.empty()) return;

    if (hide_timer) lv_timer_pause(hide_timer);

    // Create on lv_layer_top() so quick-reply popup NEVER disappears behind lockscreen or active app close!
    lv_obj_t* mbox = lv_obj_create(lv_layer_top());
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

    for (int i = 0; i <= QUICK_REPLY_COUNT; i++) {
        bool is_cancel = (i == QUICK_REPLY_COUNT);
        std::string text = is_cancel ? "Iptal" : get_quick_reply(i);

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
        snprintf(payload, 256, "%s|%s", current_banner_notif_id.c_str(), text.c_str());
        lv_obj_add_event_cb(btn, on_banner_reply_selected, LV_EVENT_CLICKED, payload);
        lv_obj_add_event_cb(btn, on_banner_user_data_deleted, LV_EVENT_DELETE, payload);

        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, text.c_str());
        lv_obj_set_width(lbl, LV_PCT(92));
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_center(lbl);
    }
}

void AppNotifications::on_banner_reply_selected(lv_event_t* e) {
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
                ESP_LOGI(TAG, "Sending BLE Quick Reply: %s", ble_cmd);
                ble_manager_send_media_command(ble_cmd);
                bsp_vibrate_pulse(50);
            }
        }
    }

    lv_obj_delete_async(mbox);
    hide();
}

extern "C" void app_notifications_update_quick_replies(const char* payload) {
    AppNotifications::update_quick_replies(payload);
}

extern "C" void app_notifications_update_nav_from_ble(const char* payload) {
    if (!payload || strlen(payload) == 0) return;

    char buf[256];
    strncpy(buf, payload, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    // Expected format: "<DIR>|<DIST>|<STREET>"
    char* p1 = strchr(buf, '|');
    if (!p1) return;
    *p1 = '\0';
    char dir_code = buf[0] ? buf[0] : 'S';

    char* rest = p1 + 1;
    char* p2 = strchr(rest, '|');
    const char* dist_str = rest;
    const char* street_str = "";
    if (p2) {
        *p2 = '\0';
        street_str = p2 + 1;
    }

    bool silent_guard = (AppPower::get_active_profile() == POWER_PROFILE_SCHOOL) ||
                        (AppPower::get_active_profile() == POWER_PROFILE_SUPER_SAVER) ||
                        AppSchool::is_lesson_in_progress();

    if (bsp_display_lock(1000)) {
        bool maneuver_changed = AppNotifications::show_nav(dir_code, dist_str, street_str);
        bsp_display_unlock();

        // Only buzz wrist & wake AMOLED when the turn maneuver actually changes!
        if (maneuver_changed && !silent_guard) {
            bsp_vibrate_pulse(dir_code == 'F' ? 220 : 120);
            display_manager_turn_on();
        }
    }
}

extern "C" void app_notifications_hide_nav_from_ble(void) {
    if (bsp_display_lock(1000)) {
        AppNotifications::hide_nav();
        bsp_display_unlock();
    }
}

extern "C" void app_notifications_show_from_ble(const char* payload) {
    char buf[256];
    strncpy(buf, payload, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char* separator = strchr(buf, '|');
    if (separator != NULL) {
        *separator = '\0';
        const char* sender = buf;
        char* rest = separator + 1;

        std::string notif_id = "";
        const char* message = rest;

        if (strncmp(rest, "ID:", 3) == 0) {
            char* second_sep = strchr(rest, '|');
            if (second_sep != NULL) {
                *second_sep = '\0';
                notif_id = std::string(rest + 3);
                message = second_sep + 1;
            }
        }

        ESP_LOGI(TAG, "Parsed Notification -> Sender: %s, ID: %s, Message: %s", sender, notif_id.c_str(), message);

        AppNotificationsCustom::push_notification(sender, message, notif_id);

        bool silent_guard = (AppPower::get_active_profile() == POWER_PROFILE_SCHOOL) ||
                            (AppPower::get_active_profile() == POWER_PROFILE_SUPER_SAVER) ||
                            AppSchool::is_lesson_in_progress();
        if (silent_guard && !display_manager_is_on()) {
            if (AppPower::get_active_profile() != POWER_PROFILE_SUPER_SAVER) {
                bsp_vibrate_pulse(60);
            }
            ESP_LOGI(TAG, "Silent/Lesson guard active: notification saved without waking AMOLED");
            return;
        }

        bsp_vibrate_pulse(90);
        display_manager_turn_on();

        if (bsp_display_lock(1000)) {
            AppNotifications::show(sender, message, notif_id);
            AppNotificationsCustom::update_ui_if_open();
            bsp_display_unlock();
        } else {
            ESP_LOGW(TAG, "Failed to acquire display lock to show notification UI");
        }
    } else {
        ESP_LOGW(TAG, "Invalid notification payload format: %s", payload);
    }
}

extern "C" void app_notifications_show_system_alert(const char* msg) {
    ESP_LOGI(TAG, "System Alert: %s", msg);
    display_manager_turn_on();
    if (bsp_display_lock(1000)) {
        AppNotifications::show("SISTEM", msg);
        bsp_display_unlock();
    } else {
        ESP_LOGW(TAG, "Failed to acquire display lock to show system alert");
    }
}
