#include "app_agenda.hpp"
#include "esp_lib_utils.h"
#include <string.h>

static AppAgenda* g_agenda_app = nullptr;

static void checkbox_event_cb(lv_event_t * e) {
    lv_obj_t * cb = (lv_obj_t *)lv_event_get_target(e);
    // When checked, we could send BLE to phone, but for now just let LVGL check it visually
    if (lv_obj_get_state(cb) & LV_STATE_CHECKED) {
        lv_obj_set_style_text_decor(cb, LV_TEXT_DECOR_STRIKETHROUGH, 0);
    } else {
        lv_obj_set_style_text_decor(cb, LV_TEXT_DECOR_NONE, 0);
    }
}

bool AppAgenda::init() {
    g_agenda_app = this;
    return true;
}

bool AppAgenda::run() {
    lv_obj_t* scr = lv_scr_act();
    _bg_obj = lv_obj_create(scr);
    lv_obj_set_size(_bg_obj, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(_bg_obj, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(_bg_obj, 0, 0);
    lv_obj_set_style_radius(_bg_obj, 0, 0);

    // Title
    _title_label = lv_label_create(_bg_obj);
    lv_label_set_text(_title_label, "AJANDA & GOREVLER");
    lv_obj_align(_title_label, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_text_color(_title_label, lv_color_hex(0xFFD700), 0);

    // List
    _list = lv_list_create(_bg_obj);
    lv_obj_set_size(_list, LV_PCT(90), LV_PCT(75));
    lv_obj_align(_list, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_bg_color(_list, lv_color_hex(0x1a1a1a), 0);
    lv_obj_set_style_border_width(_list, 0, 0);

    render_items();
    return true;
}

bool AppAgenda::back() {
    return close();
}

bool AppAgenda::close() {
    if (_bg_obj) {
        lv_obj_del(_bg_obj);
        _bg_obj = nullptr;
    }
    return true;
}

void AppAgenda::add_item(int id, const char* text) {
    AgendaItem item;
    item.id = id;
    item.text = text;
    item.is_task = (strncmp(text, "[ ] ", 4) == 0);
    
    // Check if exists
    bool found = false;
    for (auto& it : _items) {
        if (it.id == id) {
            it = item;
            found = true;
            break;
        }
    }
    if (!found) {
        _items.push_back(item);
    }

    if (_bg_obj) {
        render_items();
    }
}

void AppAgenda::clear_items() {
    _items.clear();
    if (_bg_obj) {
        render_items();
    }
}

void AppAgenda::render_items() {
    if (!_list) return;
    lv_obj_clean(_list);

    if (_items.empty()) {
        lv_obj_t* lbl = lv_label_create(_list);
        lv_label_set_text(lbl, "Liste Bos. Telefondan esitleyin.");
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x888888), 0);
        return;
    }

    for (const auto& item : _items) {
        if (item.is_task) {
            lv_obj_t * cb = lv_checkbox_create(_list);
            lv_checkbox_set_text(cb, item.text.c_str() + 4);
            lv_obj_add_event_cb(cb, checkbox_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
            lv_obj_set_style_text_color(cb, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_pad_all(cb, 10, 0);
        } else {
            lv_obj_t * btn = lv_list_add_btn(_list, LV_SYMBOL_BELL, item.text.c_str());
            lv_obj_set_style_bg_color(btn, lv_color_hex(0x1a1a1a), 0);
            lv_obj_set_style_text_color(btn, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_border_width(btn, 0, 0);
        }
    }
}

// Global hooks for BLE manager
void app_agenda_clear() {
    if (g_agenda_app) {
        g_agenda_app->clear_items();
    }
}

void app_agenda_add_item(int id, const char* text) {
    if (g_agenda_app) {
        g_agenda_app->add_item(id, text);
    }
}

void app_agenda_update_from_ble(const char* payload) {
    if (strcmp(payload, "CLEAR") == 0) {
        app_agenda_clear();
    } else {
        // Parse ID|Text
        char buf[256];
        strncpy(buf, payload, sizeof(buf)-1);
        buf[255] = 0;
        
        char* id_str = strtok(buf, "|");
        char* text = strtok(NULL, "|");
        if (id_str && text) {
            app_agenda_add_item(atoi(id_str), text);
        }
    }
}

