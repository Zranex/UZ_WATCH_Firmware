#include "app_wifi_test.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lvgl.h"
#include <string.h>
#include <stdio.h>

extern "C" const lv_image_dsc_t icon_smarthome;
extern "C" esp_err_t wifi_manager_start(void);
extern "C" bool wifi_manager_is_connected(void);
extern "C" bool wifi_manager_is_active(void);
extern "C" void wifi_manager_get_ip(char* buf);
extern "C" void wifi_manager_stop(void);

static volatile int wifi_test_state = 0;

static void wifi_test_task(void *pvParameters) {
    vTaskDelay(pdMS_TO_TICKS(1500)); // Guc dengelenmesi icin cok uzun bekleme
    esp_err_t err = wifi_manager_start();
    if (err == 0) {
        wifi_test_state = 2; // Basarili
    } else {
        wifi_test_state = -1;
    }
    vTaskDelete(NULL);
}

AppWiFiTest::AppWiFiTest() : esp_brookesia::systems::phone::App("Wi-Fi Test", &icon_smarthome, true) {}
AppWiFiTest::~AppWiFiTest() {}

bool AppWiFiTest::run() {
    lv_obj_t* scr = lv_scr_act();
    _bg_obj = lv_obj_create(scr);
    lv_obj_set_size(_bg_obj, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(_bg_obj, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(_bg_obj, 0, 0);
    
    _label_status = lv_label_create(_bg_obj);
    lv_label_set_text(_label_status, "Wi-Fi Kapali");
    lv_obj_set_style_text_color(_label_status, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(_label_status, LV_ALIGN_TOP_MID, 0, 80);

    _btn = lv_btn_create(_bg_obj);
    lv_obj_align(_btn, LV_ALIGN_CENTER, 0, 20);
    lv_obj_t* btn_lbl = lv_label_create(_btn);
    lv_label_set_text(btn_lbl, wifi_manager_is_active() ? "Wi-Fi Kapat" : "Wi-Fi AC");
    lv_obj_center(btn_lbl);

    lv_obj_add_event_cb(_btn, [](lv_event_t* e){
        AppWiFiTest* app = (AppWiFiTest*)lv_event_get_user_data(e);
        if (wifi_manager_is_active()) {
            wifi_manager_stop();
            lv_label_set_text(app->_label_status, "Wi-Fi Kapatildi.");
            lv_obj_t* lbl = lv_obj_get_child(app->_btn, 0);
            if (lbl) lv_label_set_text(lbl, "Wi-Fi AC");
        } else {
            if (wifi_test_state == 1) return;
            wifi_test_state = 1;
            lv_label_set_text(app->_label_status, "Sistem Sakinlesiyor...\n(1.5 sn)");
            lv_obj_set_style_text_color(app->_label_status, lv_color_hex(0xFFA500), 0);
            lv_obj_t* lbl = lv_obj_get_child(app->_btn, 0);
            if (lbl) lv_label_set_text(lbl, "Isleniyor...");
            
            xTaskCreatePinnedToCore(wifi_test_task, "wifi_test", 4096, NULL, 5, NULL, 1);
        }
    }, LV_EVENT_CLICKED, this);

    _timer = lv_timer_create([](lv_timer_t* t){
        AppWiFiTest* app = (AppWiFiTest*)t->user_data;
        if (!app->_bg_obj) return;

        if (wifi_test_state == 2) {
            if (wifi_manager_is_connected()) {
                char ip_buf[16] = "0.0.0.0";
                wifi_manager_get_ip(ip_buf);
                char buf[64];
                snprintf(buf, sizeof(buf), "Baglandi!\nIP: %s", ip_buf);
                lv_label_set_text(app->_label_status, buf);
                lv_obj_set_style_text_color(app->_label_status, lv_color_hex(0x00FF00), 0);
                
                lv_obj_t* lbl = lv_obj_get_child(app->_btn, 0);
                if (lbl) lv_label_set_text(lbl, "Wi-Fi Kapat");
                wifi_test_state = 0;
            } else {
                lv_label_set_text(app->_label_status, "Aga Baglaniliyor...");
            }
        } else if (wifi_test_state == -1) {
            lv_label_set_text(app->_label_status, "HATA OLUSTU!");
            lv_obj_set_style_text_color(app->_label_status, lv_color_hex(0xFF0000), 0);
            wifi_test_state = 0;
        }
    }, 500, this);

    return true;
}

bool AppWiFiTest::back() { return notifyCoreClosed(); }
bool AppWiFiTest::close() {
    if (_timer) {
        lv_timer_del(_timer);
        _timer = nullptr;
    }
    _bg_obj = nullptr;
    return true;
}
