#include "ui_notification_banner.hpp"
#include "esp_log.h"
#include <string.h>
#include "app_notifications_custom.hpp"
#include "bsp/esp-bsp.h"

extern "C" {
#include "ble_manager.h"
}

static const char *TAG = "app_notifications";

lv_obj_t* AppNotifications::container = nullptr;
lv_obj_t* AppNotifications::title_label = nullptr;
lv_obj_t* AppNotifications::message_label = nullptr;
lv_obj_t* AppNotifications::reply_btn = nullptr;
lv_timer_t* AppNotifications::hide_timer = nullptr;
std::string AppNotifications::current_banner_notif_id = "";

void AppNotifications::init() {
    // Create the container on the top layer so it floats above everything
    container = lv_obj_create(lv_layer_top());
    lv_obj_set_size(container, 320, 100);
    lv_obj_align(container, LV_ALIGN_TOP_MID, 0, -110); // Start hidden above the screen
    lv_obj_set_style_radius(container, 20, 0);
    lv_obj_set_style_bg_color(container, lv_color_hex(0x1a1a1a), 0);
    lv_obj_set_style_border_width(container, 2, 0);
    lv_obj_set_style_border_color(container, lv_color_hex(0x333333), 0);
    lv_obj_set_style_pad_all(container, 15, 0);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    
    // Add shadow
    lv_obj_set_style_shadow_width(container, 20, 0);
    lv_obj_set_style_shadow_color(container, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(container, LV_OPA_50, 0);
    lv_obj_set_style_shadow_ofs_y(container, 10, 0);

    // Title label (Sender)
    title_label = lv_label_create(container);
    lv_obj_set_width(title_label, LV_PCT(100));
    lv_obj_set_style_text_color(title_label, lv_color_hex(0x25D366), 0); // WhatsApp Greenish
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
    lv_obj_set_size(reply_btn, 80, 30);
    lv_obj_align(reply_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(reply_btn, lv_color_hex(0x0055FF), 0);
    lv_obj_add_flag(reply_btn, LV_OBJ_FLAG_HIDDEN); // Hidden by default
    lv_obj_add_event_cb(reply_btn, on_banner_reply_clicked, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t* btn_lbl = lv_label_create(reply_btn);
    lv_label_set_text(btn_lbl, "Yanitla");
    lv_obj_set_style_text_font(btn_lbl, &lv_font_montserrat_12, 0);
    lv_obj_center(btn_lbl);

    // Create a timer but pause it
    hide_timer = lv_timer_create(hide_timer_cb, 5000, NULL);
    lv_timer_pause(hide_timer);
}

void AppNotifications::show(const char* sender, const char* message, const std::string& notif_id) {
    if (!container) return;

    current_banner_notif_id = notif_id;
    lv_label_set_text(title_label, sender);
    lv_label_set_text(message_label, message);

    if (!notif_id.empty()) {
        lv_obj_clear_flag(reply_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(reply_btn, LV_OBJ_FLAG_HIDDEN);
    }

    // Slide down animation
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, container);
    lv_anim_set_values(&a, -100, 10); // From off-screen top to y=10
    lv_anim_set_time(&a, 300);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_path_cb(&a, lv_anim_path_overshoot);
    lv_anim_start(&a);

    // Restart the hide timer
    lv_timer_reset(hide_timer);
    lv_timer_resume(hide_timer);
}

void AppNotifications::hide() {
    if (!container) return;

    // Slide up animation
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, container);
    lv_anim_set_values(&a, lv_obj_get_y(container), -100);
    lv_anim_set_time(&a, 300);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_start(&a);

    lv_timer_pause(hide_timer);
}

void AppNotifications::hide_timer_cb(lv_timer_t* timer) {
    // LVGL timer callback is already within LVGL context, but we still call hide() which locks.
    // Recursive locking is supported by bsp_display_lock if the underlying mutex allows,
    // but typically it's safer not to lock from within LVGL timer callbacks if we're not sure.
    // However, AppNotifications::hide() takes the lock. If this causes a deadlock, we should separate the logic.
    // Actually, `bsp_display_lock` uses FreeRTOS recursive mutexes in esp_lcd_panel_io, but let's be safe.
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

    // Pause the hide timer so the banner doesn't disappear while replying
    if (hide_timer) lv_timer_pause(hide_timer);

    // Create a modal popup for Quick Replies
    lv_obj_t* mbox = lv_obj_create(lv_scr_act());
    lv_obj_set_size(mbox, LV_PCT(90), LV_PCT(80));
    lv_obj_center(mbox);
    lv_obj_set_style_bg_color(mbox, lv_color_hex(0x1a1a1a), 0);
    lv_obj_set_style_border_width(mbox, 2, 0);
    lv_obj_set_style_border_color(mbox, lv_color_hex(0x333333), 0);
    lv_obj_set_flex_flow(mbox, LV_FLEX_FLOW_COLUMN);

    lv_obj_t* title = lv_label_create(mbox);
    lv_label_set_text(title, "Hizli Yanit Sec:");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);

    const char* replies[] = {
        "Tamam",
        "Evet",
        "Hayir",
        "Mesgulum, sonra donerim.",
        "Sesli Mesaj (Mikrofon)",
        "Iptal"
    };

    for (int i = 0; i < 6; i++) {
        lv_obj_t* btn = lv_btn_create(mbox);
        lv_obj_set_width(btn, LV_PCT(100));
        lv_obj_set_height(btn, 40);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x333333), 0);
        
        char* payload = (char*)malloc(256);
        snprintf(payload, 256, "%s|%s", current_banner_notif_id.c_str(), replies[i]);
        lv_obj_add_event_cb(btn, on_banner_reply_selected, LV_EVENT_CLICKED, payload);
        lv_obj_add_event_cb(btn, on_banner_user_data_deleted, LV_EVENT_DELETE, payload);

        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, replies[i]);
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
                if (strcmp(reply_text, "Sesli Mesaj (Mikrofon)") == 0) {
                    ESP_LOGI(TAG, "Sesli Mesaj triggered for ID: %s", notif_id);
                    char ble_cmd[256];
                    snprintf(ble_cmd, sizeof(ble_cmd), "VOICE_REPLY|%s", notif_id);
                    ble_manager_send_media_command(ble_cmd);
                } else {
                    char ble_cmd[256];
                    snprintf(ble_cmd, sizeof(ble_cmd), "REPLY|%s|%s", notif_id, reply_text);
                    ESP_LOGI(TAG, "Sending BLE Command: %s", ble_cmd);
                    ble_manager_send_media_command(ble_cmd);
                }
            }
        }
    }

    // Close the popup and hide the banner
    lv_obj_del(mbox);
    hide();
}

extern "C" void app_notifications_show_from_ble(const char* payload) {
    // payload is in the format: "SENDER|MESSAGE"
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
        
        // Check if the rest starts with "ID:"
        if (strncmp(rest, "ID:", 3) == 0) {
            char* second_sep = strchr(rest, '|');
            if (second_sep != NULL) {
                *second_sep = '\0';
                notif_id = std::string(rest + 3); // Skip "ID:"
                message = second_sep + 1;
            }
        }
        
        ESP_LOGI(TAG, "Parsed Notification -> Sender: %s, ID: %s, Message: %s", sender, notif_id.c_str(), message);
        
        // Push notification to history (thread-safe internally)
        AppNotificationsCustom::push_notification(sender, message, notif_id);
        
        // Wait up to 1000ms for LVGL lock since we're in the BLE task
        if (bsp_display_lock(1000)) {
            // Show the drop-down banner
            AppNotifications::show(sender, message, notif_id);
            
            // Dynamically refresh the app's list if it's currently open on screen
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
    if (bsp_display_lock(1000)) {
        AppNotifications::show("SISTEM", msg);
        bsp_display_unlock();
    } else {
        ESP_LOGW(TAG, "Failed to acquire display lock to show system alert");
    }
}
