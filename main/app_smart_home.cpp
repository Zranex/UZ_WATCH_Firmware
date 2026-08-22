#include "app_smart_home.hpp"
extern const lv_image_dsc_t icon_smarthome;
#include "esp_lib_utils.h"

// Reuse the existing BLE media command function for now, since it just sends a string via 0xFF03
extern "C" void ble_manager_send_media_command(const char* command);

using namespace esp_brookesia;

AppSmartHome::AppSmartHome() : App("Akilli Ev", &icon_smarthome, true) {
    _bg_obj = nullptr;
}

AppSmartHome::~AppSmartHome() {
}

void AppSmartHome::send_command(const char* cmd) {
    ESP_UTILS_LOGI("SmartHome Command: %s", cmd);
    ble_manager_send_media_command(cmd);
}

void AppSmartHome::on_btn_clicked(lv_event_t* e) {
    AppSmartHome* app = (AppSmartHome*)lv_event_get_user_data(e);
    
    lv_obj_t* btn = (lv_obj_t*)lv_event_get_target(e);
    const char* cmd_str = (const char*)btn->user_data;
    
    if (app && cmd_str) {
        app->send_command(cmd_str);
        
        // Visual feedback (briefly change color)
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x1DB954), 0); // Green flash
    }
}

bool AppSmartHome::run() {
    ESP_UTILS_LOGI("AppSmartHome run");
    if (_bg_obj != nullptr) {
        return true;
    }
    
    _bg_obj = lv_obj_create(lv_scr_act());

    lv_obj_set_size(_bg_obj, LV_PCT(100), LV_PCT(100));
    lv_obj_center(_bg_obj);
    lv_obj_set_style_bg_color(_bg_obj, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(_bg_obj, 0, 0);
    lv_obj_set_style_radius(_bg_obj, 0, 0);
    lv_obj_clear_flag(_bg_obj, LV_OBJ_FLAG_SCROLLABLE);

    // Title
    lv_obj_t* title = lv_label_create(_bg_obj);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(title, "AKILLI EV");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 15);

    // Create a 2x2 Grid Container
    lv_obj_t* grid = lv_obj_create(_bg_obj);
    lv_obj_set_size(grid, 340, 340);
    lv_obj_align(grid, LV_ALIGN_CENTER, 0, 20);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    
    static lv_coord_t col_dsc[] = {160, 160, LV_GRID_TEMPLATE_LAST};
    static lv_coord_t row_dsc[] = {160, 160, LV_GRID_TEMPLATE_LAST};
    
    lv_obj_set_style_grid_column_dsc_array(grid, col_dsc, 0);
    lv_obj_set_style_grid_row_dsc_array(grid, row_dsc, 0);
    lv_obj_set_layout(grid, LV_LAYOUT_GRID);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    // Helper lambda to create buttons
    auto create_btn = [&](int col, int row, const char* icon, const char* name, const char* cmd, lv_color_t color) {
        lv_obj_t* btn = lv_btn_create(grid);
        lv_obj_set_grid_cell(btn, LV_GRID_ALIGN_STRETCH, col, 1, LV_GRID_ALIGN_STRETCH, row, 1);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x222222), 0);
        lv_obj_set_style_radius(btn, 20, 0);
        
        btn->user_data = (void*)cmd;
        lv_obj_add_event_cb(btn, on_btn_clicked, LV_EVENT_CLICKED, this);

        lv_obj_t* icon_label = lv_label_create(btn);
        lv_obj_set_style_text_font(icon_label, &lv_font_montserrat_40, 0);
        lv_obj_set_style_text_color(icon_label, color, 0);
        lv_label_set_text(icon_label, icon);
        lv_obj_align(icon_label, LV_ALIGN_CENTER, 0, -20);

        lv_obj_t* name_label = lv_label_create(btn);
        lv_obj_set_style_text_font(name_label, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(name_label, lv_color_hex(0xAAAAAA), 0);
        lv_label_set_text(name_label, name);
        lv_obj_align(name_label, LV_ALIGN_CENTER, 0, 30);
    };

    // Button 1: Tavan Işığı
    create_btn(0, 0, LV_SYMBOL_WIFI, "Tavan", "SMART_HOME|LIGHT_1_TOGGLE", lv_color_hex(0xFFB300));
    
    // Button 2: Masa Lambası
    create_btn(1, 0, LV_SYMBOL_BELL, "Masa", "SMART_HOME|LIGHT_2_TOGGLE", lv_color_hex(0x03A9F4));

    
    // Button 3: PC Uyut
    create_btn(0, 1, LV_SYMBOL_POWER, "PC Uyut", "SMART_HOME|PC_SLEEP", lv_color_hex(0xE53935));
    
    // Button 4: Tümü Kapat
    create_btn(1, 1, LV_SYMBOL_TRASH, "Tümü Kapat", "SMART_HOME|ALL_OFF", lv_color_hex(0x9E9E9E));

    return true;
}

bool AppSmartHome::back() {
    return close();
}

bool AppSmartHome::close() {
    ESP_UTILS_LOGI("AppSmartHome close");
    if (_bg_obj) {
        lv_obj_delete(_bg_obj);
        _bg_obj = nullptr;
    }
    return true;
}
