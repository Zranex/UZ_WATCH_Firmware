#include "app_activity.hpp"
#include "pedometer_task.h"
#include "esp_log.h"
#include <stdio.h>

#define DAILY_STEP_GOAL 10000

static const char* TAG = "AppActivity";

AppActivity::AppActivity() : esp_brookesia::systems::phone::App("Aktivite", nullptr, true) {
}

AppActivity::~AppActivity() {
}

bool AppActivity::run() {
    if (_bg_obj != nullptr) {
        return true;
    }

    lv_obj_t* parent = lv_scr_act();

    _bg_obj = lv_obj_create(parent);
    lv_obj_set_size(_bg_obj, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(_bg_obj, lv_color_hex(0x000000), 0);
    lv_obj_remove_style(_bg_obj, NULL, LV_PART_SCROLLBAR);

    // Outer Arc (Steps)
    _arc_steps = lv_arc_create(_bg_obj);
    lv_obj_set_size(_arc_steps, 320, 320); // Make it quite large
    lv_arc_set_rotation(_arc_steps, 270);
    lv_arc_set_bg_angles(_arc_steps, 0, 360);
    lv_obj_remove_style(_arc_steps, NULL, LV_PART_KNOB); 
    lv_obj_remove_flag(_arc_steps, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(_arc_steps);

    lv_obj_set_style_arc_color(_arc_steps, lv_color_hex(0x113311), LV_PART_MAIN);
    lv_obj_set_style_arc_width(_arc_steps, 30, LV_PART_MAIN);
    lv_obj_set_style_arc_color(_arc_steps, lv_color_hex(0x00FF00), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(_arc_steps, 30, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(_arc_steps, true, LV_PART_INDICATOR);

    uint32_t current_steps = pedometer_get_steps();

    lv_arc_set_range(_arc_steps, 0, DAILY_STEP_GOAL);
    lv_arc_set_value(_arc_steps, current_steps);

    // Number text
    _label_steps = lv_label_create(_bg_obj);
    lv_obj_set_style_text_color(_label_steps, lv_color_hex(0xFFFFFF), 0);
    
    // Attempting to use a larger built-in font if available.
    // If not, it falls back to default. Let's use lv_font_montserrat_40 if it's compiled,
    // otherwise fallback to whatever is default in Brookesia.
    // Actually, Brookesia often uses its own fonts. We'll just set text and scale it if possible, 
    // but standard lv_label has no scaling. We'll just rely on the default font.
    lv_label_set_text_fmt(_label_steps, "%lu", current_steps);
    lv_obj_align(_label_steps, LV_ALIGN_CENTER, 0, -10);

    // Label text
    _label_desc = lv_label_create(_bg_obj);
    lv_obj_set_style_text_color(_label_desc, lv_color_hex(0xAAAAAA), 0);
    lv_label_set_text(_label_desc, "ADIM");
    lv_obj_align_to(_label_desc, _label_steps, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);

    _timer = lv_timer_create(timer_cb, 500, this);

    return true;
}

bool AppActivity::back() {
    return close();
}

bool AppActivity::close() {
    if (_timer) {
        lv_timer_delete(_timer);
        _timer = nullptr;
    }
    if (_bg_obj) {
        lv_obj_delete(_bg_obj);
        _bg_obj = nullptr;
    }
    return true;
}

void AppActivity::timer_cb(lv_timer_t* t) {
    AppActivity* app = (AppActivity*)t->user_data;
    if (app && app->_arc_steps && app->_label_steps) {
        uint32_t current_steps = pedometer_get_steps();
        lv_arc_set_value(app->_arc_steps, current_steps);
        lv_label_set_text_fmt(app->_label_steps, "%lu", current_steps);
    }
}
