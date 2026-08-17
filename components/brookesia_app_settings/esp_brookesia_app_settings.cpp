#include <iterator>
#include <thread>
#include <unistd.h>
#include "esp_brookesia_app_settings.hpp"
#include "display_manager.h"
#include "esp_system.h"
#include "esp_heap_caps.h"

#define ESP_UTILS_LOG_TAG "BS:App:Settings"
#include "esp_lib_utils.h"

extern "C" {
#include "rtc_lib.h"
#include "wifi_manager.h"
}

#define APP_NAME "Settings"

using namespace std;
using namespace esp_brookesia::gui;
using namespace esp_brookesia::systems::phone;

namespace esp_brookesia::apps {

    Settings::Settings() :
        App({
        .name = APP_NAME,
        .launcher_icon = gui::StyleImage::IMAGE(&esp_brookesia_app_icon_launcher_settings_112_112),
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
    {}

    Settings::~Settings()
    {
        ESP_UTILS_LOGD("Destroy(@0x%p)", this);
    }

    static lv_obj_t * roller_hour;
    static lv_obj_t * roller_min;
    static lv_obj_t * roller_day;
    static lv_obj_t * roller_month;
    static lv_obj_t * roller_year;

    static void time_save_btn_event_cb(lv_event_t * e)
    {
        uint16_t h = lv_roller_get_selected(roller_hour);
        uint16_t m = lv_roller_get_selected(roller_min);
        uint16_t day = lv_roller_get_selected(roller_day) + 1;
        uint16_t month = lv_roller_get_selected(roller_month);
        uint16_t year = lv_roller_get_selected(roller_year) + 2024;

        struct tm timeinfo;
        timeinfo.tm_year = year - 1900;
        timeinfo.tm_mon = month;
        timeinfo.tm_mday = day;
        timeinfo.tm_hour = h;
        timeinfo.tm_min = m;
        timeinfo.tm_sec = 0;
        rtc_set_time(&timeinfo);
        ESP_UTILS_LOGI("Time manually set to %04d-%02d-%02d %02d:%02d", year, month+1, day, h, m);
    }

    static void brightness_slider_event_cb(lv_event_t * e)
    {
        lv_obj_t * slider = (lv_obj_t *)lv_event_get_target(e);
        int brightness = lv_slider_get_value(slider);
        display_manager_set_brightness(brightness);
    }

    static void timeout_dropdown_event_cb(lv_event_t * e)
    {
        lv_obj_t * dropdown = (lv_obj_t *)lv_event_get_target(e);
        uint16_t opt = lv_dropdown_get_selected(dropdown);
        
        uint32_t ms = 30000;
        switch(opt) {
            case 0: ms = 15000; break;
            case 1: ms = 30000; break;
            case 2: ms = 60000; break;
            case 3: ms = 0xFFFFFFFF; break; // Never
        }
        display_manager_set_timeout(ms);
    }

    static lv_obj_t * kb;
    static lv_obj_t * pwd_ta;
    static char selected_ssid[33];
    static lv_obj_t * wifi_status_label;
    static lv_timer_t * wifi_status_timer;

    static void wifi_status_update_cb(lv_timer_t * t) {
        if (!wifi_status_label) return;
        wifi_state_t state = wifi_manager_get_state();
        
        switch (state) {
            case WIFI_STATE_OFF:
                lv_label_set_text(wifi_status_label, "Durum: Kapali");
                break;
            case WIFI_STATE_INIT:
                lv_label_set_text(wifi_status_label, "Durum: Baslatiliyor...");
                break;
            case WIFI_STATE_IDLE:
                lv_label_set_text(wifi_status_label, "Durum: Bosta (Baglanmiyor)");
                break;
            case WIFI_STATE_CONNECTING:
                lv_label_set_text(wifi_status_label, "Durum: Baglanti Kuruluyor...");
                break;
            case WIFI_STATE_CONNECTED:
                lv_label_set_text(wifi_status_label, "Durum: Baglandi (IP bekleniyor)");
                break;
            case WIFI_STATE_GOT_IP: {
                char ip[32];
                wifi_manager_get_ip(ip);
                lv_label_set_text_fmt(wifi_status_label, "Durum: IP Alindi (%s)", ip);
                break;
            }
            case WIFI_STATE_FAILED:
                lv_label_set_text(wifi_status_label, "Durum: HATA!");
                break;
            default:
                lv_label_set_text(wifi_status_label, "Durum: Bilinmiyor");
                break;
        }
    }

    static void wifi_connect_btn_event_cb(lv_event_t * e) {
        const char * pwd = lv_textarea_get_text(pwd_ta);
        wifi_manager_connect(selected_ssid, pwd);
        lv_obj_del(lv_obj_get_parent(pwd_ta)); // Delete modal container
        kb = NULL;
    }
    
    static void wifi_cancel_btn_event_cb(lv_event_t * e) {
        lv_obj_del(lv_obj_get_parent(pwd_ta)); // Delete modal container
        kb = NULL;
    }

    static void wifi_network_btn_event_cb(lv_event_t * e) {
        lv_obj_t * btn = (lv_obj_t *)lv_event_get_target(e);
        const char * ssid = lv_list_get_btn_text(NULL, btn);
        strncpy(selected_ssid, ssid, 32);
        selected_ssid[32] = '\0';
        
        lv_obj_t * modal = lv_obj_create(lv_scr_act());
        lv_obj_set_size(modal, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_bg_color(modal, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(modal, LV_OPA_90, 0);
        
        lv_obj_t * label = lv_label_create(modal);
        lv_label_set_text_fmt(label, "%s icin sifre:", selected_ssid);
        lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 10);
        
        pwd_ta = lv_textarea_create(modal);
        lv_textarea_set_password_mode(pwd_ta, true);
        lv_textarea_set_one_line(pwd_ta, true);
        lv_obj_set_width(pwd_ta, LV_PCT(90));
        lv_obj_align(pwd_ta, LV_ALIGN_TOP_MID, 0, 40);
        
        lv_obj_t * conn_btn = lv_btn_create(modal);
        lv_obj_t * conn_lbl = lv_label_create(conn_btn);
        lv_label_set_text(conn_lbl, "Baglan");
        lv_obj_center(conn_lbl);
        lv_obj_align(conn_btn, LV_ALIGN_TOP_RIGHT, -10, 90);
        lv_obj_add_event_cb(conn_btn, wifi_connect_btn_event_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t * canc_btn = lv_btn_create(modal);
        lv_obj_t * canc_lbl = lv_label_create(canc_btn);
        lv_label_set_text(canc_lbl, "Iptal");
        lv_obj_center(canc_lbl);
        lv_obj_align(canc_btn, LV_ALIGN_TOP_LEFT, 10, 90);
        lv_obj_add_event_cb(canc_btn, wifi_cancel_btn_event_cb, LV_EVENT_CLICKED, NULL);
        
        kb = lv_keyboard_create(modal);
        lv_keyboard_set_textarea(kb, pwd_ta);
        lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    }

    static void wifi_scan_check_timer_cb(lv_timer_t * t) {
        lv_obj_t * list = (lv_obj_t *)t->user_data;
        char ssids[15][33];
        int num = wifi_manager_get_scanned_networks(ssids, 15);
        
        uint32_t child_cnt = lv_obj_get_child_cnt(list);
        while(child_cnt > 1) {
            lv_obj_del(lv_obj_get_child(list, 1));
            child_cnt = lv_obj_get_child_cnt(list);
        }
        
        if (num > 0) {
            lv_list_add_text(list, "Bulunan Aglar:");
            for (int i = 0; i < num; i++) {
                lv_obj_t * btn = lv_list_add_btn(list, LV_SYMBOL_WIFI, ssids[i]);
                lv_obj_add_event_cb(btn, wifi_network_btn_event_cb, LV_EVENT_CLICKED, NULL);
            }
        } else {
            lv_list_add_text(list, "Ag bulunamadi.");
        }
    }

    static void wifi_toggle_btn_event_cb(lv_event_t * e)
    {
        lv_obj_t * btn = (lv_obj_t *)lv_event_get_target(e);
        // lv_list_add_btn creates an image for the icon (child 0) and a label for the text (child 1).
        // Modifying child 0 with lv_label_set_text causes a fatal exception (LoadProhibited) because an image is not a label.
        lv_obj_t * label = lv_obj_get_child(btn, 1); 
        
            if (wifi_manager_is_active()) {
                wifi_manager_stop();
                if(label && lv_obj_check_type(label, &lv_label_class)) {
                    lv_label_set_text(label, "Wi-Fi'yi Ac (Gecici)");
                }
            } else {
                wifi_manager_start();
                if(label && lv_obj_check_type(label, &lv_label_class)) {
                    lv_label_set_text(label, "Wi-Fi'yi Kapat (Gecici)");
                }
            }
    }

    static void wifi_scan_btn_event_cb(lv_event_t * e)
    {
        lv_obj_t * list = (lv_obj_t *)lv_event_get_user_data(e);
        
        uint32_t child_cnt = lv_obj_get_child_cnt(list);
        while(child_cnt > 1) {
            lv_obj_del(lv_obj_get_child(list, 1));
            child_cnt = lv_obj_get_child_cnt(list);
        }

        lv_list_add_text(list, "Taraniyor... Lutfen bekleyin");
        wifi_manager_scan();
        
        lv_timer_t * timer = lv_timer_create(wifi_scan_check_timer_cb, 3500, list);
        lv_timer_set_repeat_count(timer, 1);
    }

    bool Settings::run()
    {
        ESP_UTILS_LOGD("Run Settings");

        lv_obj_t * screen = lv_scr_act();
        lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);

        // --- TABVIEW ---
        lv_obj_t * tabview = lv_tabview_create(screen);
        lv_tabview_set_tab_bar_position(tabview, LV_DIR_TOP);
        lv_tabview_set_tab_bar_size(tabview, 50);

        lv_obj_t * tab_disp = lv_tabview_add_tab(tabview, "Ekran");
        lv_obj_t * tab_wifi = lv_tabview_add_tab(tabview, "WiFi");
        lv_obj_t * tab_time = lv_tabview_add_tab(tabview, "Saat");
        lv_obj_t * tab_info = lv_tabview_add_tab(tabview, "Bilgi");

        // --- TAB 1: EKRAN AYARLARI ---
        lv_obj_set_style_bg_color(tab_disp, lv_color_hex(0x000000), 0);
        lv_obj_set_flex_flow(tab_disp, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(tab_disp, 10, 0);

        lv_obj_t * label_br = lv_label_create(tab_disp);
        lv_label_set_text(label_br, "Parlaklik");
        lv_obj_set_style_text_color(label_br, lv_color_hex(0xFFFFFF), 0);

        lv_obj_t * slider = lv_slider_create(tab_disp);
        lv_obj_set_width(slider, LV_PCT(100));
        lv_slider_set_range(slider, 10, 100);
        lv_slider_set_value(slider, display_manager_get_brightness(), LV_ANIM_OFF);
        lv_obj_add_event_cb(slider, brightness_slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

        lv_obj_t * spacer = lv_obj_create(tab_disp);
        lv_obj_set_size(spacer, 10, 20);
        lv_obj_set_style_bg_opa(spacer, 0, 0);
        lv_obj_set_style_border_width(spacer, 0, 0);

        lv_obj_t * label_to = lv_label_create(tab_disp);
        lv_label_set_text(label_to, "Kapanma Suresi");
        lv_obj_set_style_text_color(label_to, lv_color_hex(0xFFFFFF), 0);

        lv_obj_t * dd = lv_dropdown_create(tab_disp);
        lv_dropdown_set_options(dd, "15 Saniye\n30 Saniye\n1 Dakika\nHicbir Zaman");
        lv_obj_set_width(dd, LV_PCT(100));
        
        uint32_t current_to = display_manager_get_timeout();
        if (current_to <= 15000) lv_dropdown_set_selected(dd, 0);
        else if (current_to <= 30000) lv_dropdown_set_selected(dd, 1);
        else if (current_to <= 60000) lv_dropdown_set_selected(dd, 2);
        else lv_dropdown_set_selected(dd, 3);
        lv_obj_add_event_cb(dd, timeout_dropdown_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

        // --- TAB 2: WIFI (Faz 1 İskelet/Mock) ---
        lv_obj_set_style_bg_color(tab_wifi, lv_color_hex(0x000000), 0);
        lv_obj_set_flex_flow(tab_wifi, LV_FLEX_FLOW_COLUMN);
        
        wifi_status_label = lv_label_create(tab_wifi);
        lv_label_set_text(wifi_status_label, "Durum: Bilinmiyor");
        lv_obj_set_style_text_color(wifi_status_label, lv_color_hex(0x00FF00), 0);
        lv_obj_set_width(wifi_status_label, LV_PCT(100));
        lv_obj_set_style_pad_all(wifi_status_label, 10, 0);

        lv_obj_t * wifi_list = lv_list_create(tab_wifi);
        lv_obj_set_size(wifi_list, LV_PCT(100), LV_PCT(85));
        lv_obj_set_style_bg_color(wifi_list, lv_color_hex(0x000000), 0);
        lv_obj_set_style_border_width(wifi_list, 0, 0);
        lv_obj_set_style_text_color(wifi_list, lv_color_hex(0x000000), 0);

        lv_obj_t * toggle_btn = lv_list_add_btn(wifi_list, LV_SYMBOL_POWER, wifi_manager_is_active() ? "Wi-Fi'yi Kapat (Gecici)" : "Wi-Fi'yi Ac (Gecici)");
        lv_obj_add_event_cb(toggle_btn, wifi_toggle_btn_event_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t * scan_btn = lv_list_add_btn(wifi_list, LV_SYMBOL_REFRESH, "Aglari Tara");
        lv_obj_add_event_cb(scan_btn, wifi_scan_btn_event_cb, LV_EVENT_CLICKED, wifi_list);
        
        wifi_status_timer = lv_timer_create(wifi_status_update_cb, 1000, NULL);
        wifi_status_update_cb(wifi_status_timer);

        // --- TAB 3: SAAT (TIME SETTINGS) ---
        lv_obj_set_style_bg_color(tab_time, lv_color_hex(0x000000), 0);
        lv_obj_set_flex_flow(tab_time, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(tab_time, 10, 0);

        lv_obj_t * time_container = lv_obj_create(tab_time);
        lv_obj_set_width(time_container, LV_PCT(100));
        lv_obj_set_style_bg_color(time_container, lv_color_hex(0x000000), 0);
        lv_obj_set_style_border_width(time_container, 0, 0);
        lv_obj_set_flex_flow(time_container, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(time_container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        const char* hours_opts = "00\n01\n02\n03\n04\n05\n06\n07\n08\n09\n10\n11\n12\n13\n14\n15\n16\n17\n18\n19\n20\n21\n22\n23";
        const char* mins_opts = "00\n01\n02\n03\n04\n05\n06\n07\n08\n09\n10\n11\n12\n13\n14\n15\n16\n17\n18\n19\n20\n21\n22\n23\n24\n25\n26\n27\n28\n29\n30\n31\n32\n33\n34\n35\n36\n37\n38\n39\n40\n41\n42\n43\n44\n45\n46\n47\n48\n49\n50\n51\n52\n53\n54\n55\n56\n57\n58\n59";

        roller_hour = lv_roller_create(time_container);
        lv_roller_set_options(roller_hour, hours_opts, LV_ROLLER_MODE_NORMAL);
        lv_roller_set_visible_row_count(roller_hour, 3);
        
        lv_obj_t * label_colon = lv_label_create(time_container);
        lv_label_set_text(label_colon, ":");
        lv_obj_set_style_text_color(label_colon, lv_color_hex(0xFFFFFF), 0);
        
        roller_min = lv_roller_create(time_container);
        lv_roller_set_options(roller_min, mins_opts, LV_ROLLER_MODE_NORMAL);
        lv_roller_set_visible_row_count(roller_min, 3);

        lv_obj_t * date_container = lv_obj_create(tab_time);
        lv_obj_set_width(date_container, LV_PCT(100));
        lv_obj_set_style_bg_color(date_container, lv_color_hex(0x000000), 0);
        lv_obj_set_style_border_width(date_container, 0, 0);
        lv_obj_set_flex_flow(date_container, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(date_container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        const char* days_opts = "01\n02\n03\n04\n05\n06\n07\n08\n09\n10\n11\n12\n13\n14\n15\n16\n17\n18\n19\n20\n21\n22\n23\n24\n25\n26\n27\n28\n29\n30\n31";
        const char* months_opts = "01\n02\n03\n04\n05\n06\n07\n08\n09\n10\n11\n12";
        const char* years_opts = "2024\n2025\n2026\n2027\n2028\n2029\n2030\n2031\n2032\n2033\n2034\n2035";

        roller_day = lv_roller_create(date_container);
        lv_roller_set_options(roller_day, days_opts, LV_ROLLER_MODE_NORMAL);
        lv_roller_set_visible_row_count(roller_day, 3);
        
        lv_obj_t * label_slash1 = lv_label_create(date_container);
        lv_label_set_text(label_slash1, "/");
        lv_obj_set_style_text_color(label_slash1, lv_color_hex(0xFFFFFF), 0);
        
        roller_month = lv_roller_create(date_container);
        lv_roller_set_options(roller_month, months_opts, LV_ROLLER_MODE_NORMAL);
        lv_roller_set_visible_row_count(roller_month, 3);

        lv_obj_t * label_slash2 = lv_label_create(date_container);
        lv_label_set_text(label_slash2, "/");
        lv_obj_set_style_text_color(label_slash2, lv_color_hex(0xFFFFFF), 0);
        
        roller_year = lv_roller_create(date_container);
        lv_roller_set_options(roller_year, years_opts, LV_ROLLER_MODE_NORMAL);
        lv_roller_set_visible_row_count(roller_year, 3);

        struct tm cur_time;
        if (rtc_get_time(&cur_time) == ESP_OK) {
            lv_roller_set_selected(roller_hour, cur_time.tm_hour, LV_ANIM_OFF);
            lv_roller_set_selected(roller_min, cur_time.tm_min, LV_ANIM_OFF);
            lv_roller_set_selected(roller_day, cur_time.tm_mday > 0 ? cur_time.tm_mday - 1 : 0, LV_ANIM_OFF);
            lv_roller_set_selected(roller_month, cur_time.tm_mon, LV_ANIM_OFF);
            int y_idx = (cur_time.tm_year + 1900) - 2024;
            if (y_idx < 0) y_idx = 0;
            if (y_idx > 11) y_idx = 11;
            lv_roller_set_selected(roller_year, y_idx, LV_ANIM_OFF);
        }

        lv_obj_t * save_btn = lv_btn_create(tab_time);
        lv_obj_set_width(save_btn, LV_PCT(100));
        lv_obj_add_event_cb(save_btn, time_save_btn_event_cb, LV_EVENT_CLICKED, NULL);
        
        lv_obj_t * save_label = lv_label_create(save_btn);
        lv_label_set_text(save_label, "Saati Kaydet");
        lv_obj_center(save_label);

        // --- TAB 4: BİLGİ ---
        lv_obj_set_style_bg_color(tab_info, lv_color_hex(0x000000), 0);
        lv_obj_t * label_info = lv_label_create(tab_info);
        
        char info[128];
        uint32_t free_ram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024;
        uint32_t free_spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024;
        snprintf(info, sizeof(info), "UZ WATCH v5.0\nFaz 1 Arayuzu\n\nRAM: %lu KB\nSPIRAM: %lu KB", free_ram, free_spiram);
        
        lv_label_set_text(label_info, info);
        lv_obj_set_style_text_color(label_info, lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(label_info);

        return true;
    }

    bool Settings::back()
    {
        ESP_UTILS_LOGD("Back");
        ESP_UTILS_CHECK_FALSE_RETURN(notifyCoreClosed(), false, "Notify core closed failed");
        return true;
    }

    bool Settings::close()
    {
        ESP_UTILS_LOGD("Close");
        if (wifi_status_timer) {
            lv_timer_delete(wifi_status_timer);
            wifi_status_timer = NULL;
        }
        wifi_status_label = NULL;
        return true;
    }

    bool Settings::init()
    {
        ESP_UTILS_LOGD("Init");
        return true;
    }

    bool Settings::deinit()
    {
        ESP_UTILS_LOGD("Deinit");
        return true;
    }

    ESP_UTILS_REGISTER_PLUGIN(systems::base::App, Settings, APP_NAME)
}