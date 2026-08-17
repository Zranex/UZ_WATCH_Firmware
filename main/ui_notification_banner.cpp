#include "ui_notification_banner.hpp"
#include "esp_log.h"
#include <string.h>
#include "app_notifications_custom.hpp"
#include "bsp/esp-bsp.h"

static const char *TAG = "app_notifications";

lv_obj_t* AppNotifications::container = nullptr;
lv_obj_t* AppNotifications::title_label = nullptr;
lv_obj_t* AppNotifications::message_label = nullptr;
lv_timer_t* AppNotifications::hide_timer = nullptr;

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

    // Create a timer but pause it
    hide_timer = lv_timer_create(hide_timer_cb, 5000, NULL);
    lv_timer_pause(hide_timer);
}

void AppNotifications::show(const char* sender, const char* message) {
    if (!container) return;

    lv_label_set_text(title_label, sender);
    lv_label_set_text(message_label, message);

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

extern "C" void app_notifications_show_from_ble(const char* payload) {
    // payload is in the format: "SENDER|MESSAGE"
    char buf[256];
    strncpy(buf, payload, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char* separator = strchr(buf, '|');
    if (separator != NULL) {
        *separator = '\0';
        const char* sender = buf;
        const char* message = separator + 1;
        
        ESP_LOGI(TAG, "Parsed Notification -> Sender: %s, Message: %s", sender, message);
        
        // Push notification to history (thread-safe internally)
        AppNotificationsCustom::push_notification(sender, message);
        
        // Wait up to 1000ms for LVGL lock since we're in the BLE task
        if (bsp_display_lock(1000)) {
            // Show the drop-down banner
            AppNotifications::show(sender, message);
            
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
