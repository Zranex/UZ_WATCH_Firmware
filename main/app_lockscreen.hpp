#pragma once

#include "lvgl.h"
#include "esp_brookesia.hpp"

class AppLockscreen {
public:
    static constexpr size_t WALLPAPER_WIDTH = 410;
    static constexpr size_t WALLPAPER_HEIGHT = 502;
    static constexpr size_t WALLPAPER_BYTES = WALLPAPER_WIDTH * WALLPAPER_HEIGHT * 2; // 411640 bytes

    static void show(esp_brookesia::systems::phone::Phone* phone);
    static void show_again();

    // Custom SD-backed Lockscreen Wallpaper (stored in PSRAM, zero internal SRAM cost)
    static bool load_custom_from_sd();
    static void reset_to_default();
    static bool has_custom_wallpaper();

private:
    static lv_obj_t* lock_scr;
    static lv_obj_t* bg_img;
    static esp_brookesia::systems::phone::Phone* _phone;

    static lv_obj_t* label_hour;
    static lv_obj_t* label_minute;
    static lv_obj_t* label_second;
    static lv_obj_t* label_date;
    static lv_timer_t* timer_clock;

    static void event_cb(lv_event_t * e);
    static void anim_deleted_cb(lv_anim_t * a);
    static void timer_cb(lv_timer_t * t);
};

#ifdef __cplusplus
extern "C" {
#endif

bool app_lockscreen_reload_custom(void);
void app_lockscreen_reset_to_default(void);

#ifdef __cplusplus
}
#endif
