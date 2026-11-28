#include "app_lockscreen.hpp"
#include "esp_lib_utils.h"
#include "bsp/esp-bsp.h"
#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "esp_heap_caps.h"
extern "C" {
#include "rtc_lib.h"
}
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CUSTOM_WALLPAPER_PATH BSP_SD_MOUNT_POINT "/lockscreen_custom.rgb565"

extern const lv_image_dsc_t lockscreen_bg;
extern const lv_font_t font_cinzel_bold_160;
extern const lv_font_t font_cinzel_bold_54;
extern const lv_font_t font_cinzel_bold_36;

static uint8_t* s_custom_buf = nullptr;
static bool s_has_custom_bg = false;

static lv_image_dsc_t s_custom_dsc = {
    .header = {
        .magic = LV_IMAGE_HEADER_MAGIC,
        .cf = LV_COLOR_FORMAT_RGB565,
        .flags = 0,
        .w = AppLockscreen::WALLPAPER_WIDTH,
        .h = AppLockscreen::WALLPAPER_HEIGHT,
        .stride = AppLockscreen::WALLPAPER_WIDTH * 2,
        .reserved_2 = 0
    },
    .data_size = AppLockscreen::WALLPAPER_BYTES,
    .data = nullptr,
    .reserved = NULL
};

lv_obj_t* AppLockscreen::lock_scr = nullptr;
lv_obj_t* AppLockscreen::bg_img = nullptr;
esp_brookesia::systems::phone::Phone* AppLockscreen::_phone = nullptr;

lv_obj_t* AppLockscreen::label_hour = nullptr;
lv_obj_t* AppLockscreen::label_minute = nullptr;
lv_obj_t* AppLockscreen::label_second = nullptr;
lv_obj_t* AppLockscreen::label_date = nullptr;
lv_timer_t* AppLockscreen::timer_clock = nullptr;

bool AppLockscreen::has_custom_wallpaper() {
    return s_has_custom_bg && (s_custom_buf != nullptr);
}

bool AppLockscreen::load_custom_from_sd() {
    struct stat st = {0};
    if (stat(CUSTOM_WALLPAPER_PATH, &st) != 0 || (size_t)st.st_size != WALLPAPER_BYTES) {
        return false;
    }

    FILE* f = fopen(CUSTOM_WALLPAPER_PATH, "rb");
    if (!f) {
        ESP_UTILS_LOGW("Could not open custom lockscreen wallpaper: %s", CUSTOM_WALLPAPER_PATH);
        return false;
    }

    // Read into temporary PSRAM buffer outside LVGL lock so UI stays 60fps
    uint8_t* tmp = (uint8_t*)heap_caps_malloc(WALLPAPER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!tmp) {
        fclose(f);
        ESP_UTILS_LOGE("PSRAM allocation failed for custom wallpaper staging");
        return false;
    }

    size_t read_total = fread(tmp, 1, WALLPAPER_BYTES, f);
    fclose(f);
    if (read_total != WALLPAPER_BYTES) {
        free(tmp);
        ESP_UTILS_LOGW("Incomplete custom wallpaper read (%u/%u)", (unsigned)read_total, (unsigned)WALLPAPER_BYTES);
        return false;
    }

    if (!s_custom_buf) {
        s_custom_buf = (uint8_t*)heap_caps_malloc(WALLPAPER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_custom_buf) {
            free(tmp);
            ESP_UTILS_LOGE("PSRAM allocation failed for permanent custom wallpaper");
            return false;
        }
    }

    if (bsp_display_lock(1500)) {
        memcpy(s_custom_buf, tmp, WALLPAPER_BYTES);
        s_custom_dsc.data = s_custom_buf;
        s_has_custom_bg = true;
        lv_image_cache_drop(&s_custom_dsc);
        if (bg_img && lock_scr) {
            lv_image_set_src(bg_img, &s_custom_dsc);
            lv_obj_invalidate(bg_img);
        }
        bsp_display_unlock();
    } else {
        free(tmp);
        ESP_UTILS_LOGW("Skipped live custom wallpaper swap: display lock busy");
        return false;
    }

    free(tmp);
    ESP_UTILS_LOGI("Loaded custom lockscreen wallpaper from SD (%u bytes in PSRAM)", (unsigned)WALLPAPER_BYTES);
    return true;
}

void AppLockscreen::reset_to_default() {
    unlink(CUSTOM_WALLPAPER_PATH);
    if (bsp_display_lock(1000)) {
        s_has_custom_bg = false;
        if (bg_img && lock_scr) {
            lv_image_set_src(bg_img, &lockscreen_bg);
            lv_obj_invalidate(bg_img);
        }
        bsp_display_unlock();
    } else {
        s_has_custom_bg = false;
    }
    ESP_UTILS_LOGI("Reverted lockscreen wallpaper to default.");
}

void AppLockscreen::anim_deleted_cb(lv_anim_t * a) {
    if (lock_scr) {
        lv_obj_delete_async(lock_scr);
        lock_scr = nullptr;
        bg_img = nullptr;
        ESP_UTILS_LOGI("Lockscreen object deleted.");
    }
}

static int s_last_hour = -1;
static int s_last_min  = -1;
static int s_last_sec  = -1;
static int s_last_mday = -1;

void AppLockscreen::timer_cb(lv_timer_t * t) {
    if (!lock_scr || !label_hour || !label_minute || !label_second || !label_date) return;

    struct tm timeinfo;
    if (rtc_get_time(&timeinfo) == ESP_OK) {
        char buf[32];

        // Diff-guarded updates: never re-decompress 160px Cinzel hour/min glyphs when only seconds tick!
        if (timeinfo.tm_hour != s_last_hour) {
            s_last_hour = timeinfo.tm_hour;
            snprintf(buf, sizeof(buf), "%02d", timeinfo.tm_hour);
            lv_label_set_text(label_hour, buf);
        }
        if (timeinfo.tm_min != s_last_min) {
            s_last_min = timeinfo.tm_min;
            snprintf(buf, sizeof(buf), "%02d", timeinfo.tm_min);
            lv_label_set_text(label_minute, buf);
        }
        if (timeinfo.tm_sec != s_last_sec) {
            s_last_sec = timeinfo.tm_sec;
            snprintf(buf, sizeof(buf), "%02d", timeinfo.tm_sec);
            lv_label_set_text(label_second, buf);
        }
        if (timeinfo.tm_mday != s_last_mday) {
            s_last_mday = timeinfo.tm_mday;
            snprintf(buf, sizeof(buf), "%02d/%02d/%d", timeinfo.tm_mday, timeinfo.tm_mon + 1, timeinfo.tm_year + 1900);
            lv_label_set_text(label_date, buf);
        }
    }
}

void AppLockscreen::event_cb(lv_event_t * e) {
    if (!lock_scr) return;

    lv_event_code_t code = lv_event_get_code(e);

    bool do_unlock = false;
    if (code == LV_EVENT_GESTURE) {
        lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
        if (dir == LV_DIR_TOP || dir == LV_DIR_BOTTOM || dir == LV_DIR_LEFT || dir == LV_DIR_RIGHT) {
            do_unlock = true;
        }
    } else if (code == LV_EVENT_SHORT_CLICKED || code == LV_EVENT_CLICKED) {
        do_unlock = true;
    }

    if (do_unlock) {
        ESP_UTILS_LOGI("Unlock event detected. Loading main screen...");

        if (timer_clock) {
            lv_timer_delete(timer_clock);
            timer_clock = nullptr;
        }

        if (_phone) {
            lv_screen_load(_phone->getDisplay().getMainScreen());
            ESP_UTILS_LOGI("Switched to main screen.");
        }

        if (lock_scr) {
            lv_obj_t* scr_to_del = lock_scr;
            lock_scr = nullptr;
            bg_img = nullptr;
            label_hour = nullptr;
            label_minute = nullptr;
            label_second = nullptr;
            label_date = nullptr;
            lv_obj_delete_async(scr_to_del);
        }
    }
}

void AppLockscreen::show_again() {
    if (_phone) {
        show(_phone);
    }
}

void AppLockscreen::show(esp_brookesia::systems::phone::Phone* phone) {
    if (lock_scr != nullptr) return;
    _phone = phone;

    lock_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(lock_scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(lock_scr, LV_OPA_COVER, 0);
    lv_obj_remove_style(lock_scr, NULL, LV_PART_SCROLLBAR);

    bg_img = lv_image_create(lock_scr);
    if (s_has_custom_bg && s_custom_buf != nullptr) {
        lv_image_set_src(bg_img, &s_custom_dsc);
    } else {
        lv_image_set_src(bg_img, &lockscreen_bg);
    }
    lv_obj_center(bg_img);
    lv_obj_remove_flag(bg_img, LV_OBJ_FLAG_CLICKABLE);

    label_hour = lv_label_create(lock_scr);
    lv_obj_set_style_text_font(label_hour, &font_cinzel_bold_160, 0);
    lv_obj_set_style_text_color(label_hour, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(label_hour, "00");
    lv_obj_align(label_hour, LV_ALIGN_CENTER, 0, -95);

    label_minute = lv_label_create(lock_scr);
    lv_obj_set_style_text_font(label_minute, &font_cinzel_bold_160, 0);
    lv_obj_set_style_text_color(label_minute, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(label_minute, "00");
    lv_obj_align_to(label_minute, label_hour, LV_ALIGN_OUT_BOTTOM_MID, 10, 0);

    label_second = lv_label_create(lock_scr);
    lv_obj_set_style_text_font(label_second, &font_cinzel_bold_54, 0);
    lv_obj_set_style_text_color(label_second, lv_color_hex(0xDDDDDD), 0);
    lv_label_set_text(label_second, "00");
    lv_obj_align_to(label_second, label_minute, LV_ALIGN_OUT_RIGHT_BOTTOM, 5, -15);

    label_date = lv_label_create(lock_scr);
    lv_obj_set_style_text_font(label_date, &font_cinzel_bold_36, 0);
    lv_obj_set_style_text_color(label_date, lv_color_hex(0xAAAAAA), 0);
    lv_label_set_text(label_date, "01/01/2026");
    lv_obj_align_to(label_date, label_minute, LV_ALIGN_OUT_BOTTOM_MID, 0, 15);

    s_last_hour = -1;
    s_last_min  = -1;
    s_last_sec  = -1;
    s_last_mday = -1;
    timer_clock = lv_timer_create(timer_cb, 1000, nullptr);
    timer_cb(timer_clock);

    lv_obj_add_flag(lock_scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(lock_scr, event_cb, LV_EVENT_GESTURE, nullptr);
    lv_obj_add_event_cb(lock_scr, event_cb, LV_EVENT_SHORT_CLICKED, nullptr);
    lv_obj_add_event_cb(lock_scr, event_cb, LV_EVENT_CLICKED, nullptr);

    lv_screen_load(lock_scr);
    ESP_UTILS_LOGI("Lockscreen loaded (%s).", s_has_custom_bg ? "custom wallpaper" : "default wallpaper");
}

extern "C" bool app_lockscreen_reload_custom(void) {
    return AppLockscreen::load_custom_from_sd();
}

extern "C" void app_lockscreen_reset_to_default(void) {
    AppLockscreen::reset_to_default();
}
