#include <iterator>
#include <thread>
#include <unistd.h>
#include "app_watchface.hpp"
#include "app_watchface.hpp"
extern "C" {
#include "rtc_lib.h"
}

#define ESP_UTILS_LOG_TAG "App:Watchface"
#include "esp_lib_utils.h"

#define APP_NAME "Clock"

using namespace std;
using namespace esp_brookesia::gui;
using namespace esp_brookesia::systems::phone;

namespace esp_brookesia::apps {

    Watchface::Watchface() :
        App({
        .name = APP_NAME,
        .launcher_icon = gui::StyleImage::IMAGE(&app_watchface_128_128),
        .screen_size = gui::StyleSize::RECT_PERCENT(100, 100),
        .flags = {
            .enable_default_screen = 1,
            .enable_recycle_resource = 0,
            .enable_resize_visual_area = 1,
        },
            },
        {
        .app_launcher_page_index = 0,
        .flags = {
            .enable_navigation_gesture = 1,
        },
        })
    {
    }

    Watchface::~Watchface()
    {
        ESP_UTILS_LOGD("Destroy(@0x%p)", this);
    }

    void Watchface::timer_cb(lv_timer_t* t) {
        Watchface* wf = (Watchface*)t->user_data;
        if (!wf) return;
        
        struct tm timeinfo;
        if (rtc_get_time(&timeinfo) == ESP_OK) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%02d", timeinfo.tm_hour);
            if (wf->label_hour) lv_label_set_text(wf->label_hour, buf);
            
            snprintf(buf, sizeof(buf), "%02d", timeinfo.tm_min);
            if (wf->label_minute) lv_label_set_text(wf->label_minute, buf);
            
            snprintf(buf, sizeof(buf), "%02d", timeinfo.tm_sec);
            if (wf->label_second) lv_label_set_text(wf->label_second, buf);
            
            const char* wday = rtc_get_weekday_short_string();
            if (wf->label_weekday) lv_label_set_text(wf->label_weekday, wday);
            
            snprintf(buf, sizeof(buf), "%02d/%02d", timeinfo.tm_mday, timeinfo.tm_mon + 1);
            if (wf->label_date) lv_label_set_text(wf->label_date, buf);
        }
    }

    bool Watchface::run()
    {
        ESP_UTILS_LOGD("Run");


        lv_obj_t* watchface_screen = lv_obj_create(lv_scr_act());
        lv_obj_remove_style_all(watchface_screen);
        lv_obj_set_size(watchface_screen, lv_pct(100), lv_pct(100));

        lv_obj_set_style_bg_color(watchface_screen, lv_color_hex(0x000000), 0);

        lv_obj_t* image = lv_image_create(watchface_screen);
        lv_image_set_src(image, &background_wf);
        lv_obj_set_align(image, LV_ALIGN_CENTER);

        label_hour = lv_label_create(watchface_screen);
        lv_obj_set_y(label_hour, -95);
        lv_obj_set_align(label_hour, LV_ALIGN_CENTER);
        lv_label_set_text(label_hour, "--");
        lv_obj_set_style_text_letter_space(label_hour, 1, 0);
        lv_obj_set_style_text_font(label_hour, &font_numbers_160, 0);
        lv_obj_set_style_text_color(label_hour, lv_color_hex(0xF0B000), LV_PART_MAIN | LV_STATE_DEFAULT);

        label_minute = lv_label_create(watchface_screen);
        lv_obj_set_y(label_minute, 105);
        lv_obj_set_align(label_minute, LV_ALIGN_CENTER);
        lv_label_set_text(label_minute, "--");
        lv_obj_set_style_text_letter_space(label_minute, 1, 0);
        lv_obj_set_style_text_font(label_minute, &font_numbers_160, 0);
        lv_obj_set_style_text_color(label_minute, lv_color_hex(0x90F090), LV_PART_MAIN | LV_STATE_DEFAULT);

        label_second = lv_label_create(watchface_screen);
        lv_obj_set_align(label_second, LV_ALIGN_CENTER);
        lv_obj_set_x(label_second, 60); // Saniyeyi saga kaydir
        lv_obj_set_y(label_second, 0); // Ortala
        lv_label_set_text(label_second, "--");
        lv_obj_set_style_text_letter_space(label_second, 1, 0);
        lv_obj_set_style_text_font(label_second, &font_numbers_80, 0);
        lv_obj_set_style_text_color(label_second, lv_color_hex(0x909090), LV_PART_MAIN | LV_STATE_DEFAULT);

        lv_obj_t* date_cont = lv_obj_create(watchface_screen);
        lv_obj_remove_style_all(date_cont);
        lv_obj_set_size(date_cont, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_align(date_cont, LV_ALIGN_BOTTOM_MID); // Tarihi asagi ortaya al
        lv_obj_set_y(date_cont, -40); // Biraz yukari kaldir
        lv_obj_set_flex_flow(date_cont, LV_FLEX_FLOW_COLUMN);


        label_date = lv_label_create(date_cont);
        lv_label_set_text(label_date, "--/--");
        lv_obj_set_style_text_letter_space(label_date, 1, 0);
        lv_obj_set_style_text_font(label_date, &font_normal_32, 0);
        lv_obj_set_style_text_color(label_date, lv_color_hex(0xc0c0c0), LV_PART_MAIN | LV_STATE_DEFAULT);

        label_weekday = lv_label_create(date_cont);
        lv_label_set_text(label_weekday, "---");
        lv_obj_set_style_text_letter_space(label_weekday, 3, 0);
        lv_obj_set_style_text_font(label_weekday, &font_bold_32, 0);
        lv_obj_set_style_text_color(label_weekday, lv_color_hex(0xc0c0c0), LV_PART_MAIN | LV_STATE_DEFAULT);

        timer_clock = lv_timer_create(timer_cb, 1000, this);
        timer_cb(timer_clock);

        return true;
    }

    bool Watchface::back()
    {
        ESP_UTILS_LOGD("Back");
        ESP_UTILS_CHECK_FALSE_RETURN(notifyCoreClosed(), false, "Notify core closed failed");
        return true;
    }

    bool Watchface::close()
    {
        ESP_UTILS_LOGD("Close");
        if (timer_clock) {
            lv_timer_delete(timer_clock);
            timer_clock = nullptr;
        }
        return true;
    }

    bool Watchface::init()
    {
        ESP_UTILS_LOGD("Init");

        return true;
    }

    bool Watchface::deinit()
    {
        ESP_UTILS_LOGD("Deinit");

        return true;
    }


    ESP_UTILS_REGISTER_PLUGIN(systems::base::App, Watchface, APP_NAME)

}