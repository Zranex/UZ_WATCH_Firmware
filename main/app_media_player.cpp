#include "app_media_player.hpp"
#include <vector>
#include <sstream>
#include "esp_lib_utils.h"

using namespace esp_brookesia;

AppMediaPlayer* AppMediaPlayer::_instance = nullptr;

AppMediaPlayer::AppMediaPlayer() : App("Şimdi Çalıyor", nullptr, true) {
    _bg_obj = nullptr;
    _label_source = nullptr;
    _label_title = nullptr;
    _label_artist = nullptr;
    _btn_prev = nullptr;
    _btn_play = nullptr;
    _btn_next = nullptr;
    _label_play_icon = nullptr;
    _current_state = "PAUSED";
    _instance = this;
}

AppMediaPlayer::~AppMediaPlayer() {
    if (_instance == this) {
        _instance = nullptr;
    }
}

bool AppMediaPlayer::run() {
    ESP_UTILS_LOGI("AppMediaPlayer run");
    
    // Create main background object
    _bg_obj = lv_obj_create(lv_scr_act());
    lv_obj_set_size(_bg_obj, LV_PCT(100), LV_PCT(100));
    lv_obj_center(_bg_obj);
    lv_obj_set_style_bg_color(_bg_obj, lv_color_hex(0x121212), 0);
    lv_obj_set_style_border_width(_bg_obj, 5, 0); // Border for source color
    lv_obj_set_style_border_color(_bg_obj, lv_color_hex(0x333333), 0);
    lv_obj_clear_flag(_bg_obj, LV_OBJ_FLAG_SCROLLABLE);

    // Source Label (Top)
    _label_source = lv_label_create(_bg_obj);
    lv_obj_set_style_text_font(_label_source, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(_label_source, lv_color_hex(0x888888), 0);
    lv_label_set_text(_label_source, "BEKLENİYOR...");
    lv_obj_align(_label_source, LV_ALIGN_TOP_MID, 0, 20);

    // Title Label (Center-ish)
    _label_title = lv_label_create(_bg_obj);
    lv_obj_set_style_text_font(_label_title, &lv_font_montserrat_40, 0); // Larger font
    lv_obj_set_style_text_color(_label_title, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(_label_title, "Müzik Çalmıyor");
    lv_label_set_long_mode(_label_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(_label_title, 350);
    lv_obj_set_style_text_align(_label_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(_label_title, LV_ALIGN_CENTER, 0, -40);

    // Artist Label (Below Title)
    _label_artist = lv_label_create(_bg_obj);
    lv_obj_set_style_text_font(_label_artist, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(_label_artist, lv_color_hex(0xAAAAAA), 0);
    lv_label_set_text(_label_artist, "-");
    lv_label_set_long_mode(_label_artist, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(_label_artist, 300);
    lv_obj_set_style_text_align(_label_artist, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align_to(_label_artist, _label_title, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);

    // Play/Pause Button (Center Bottom)
    _btn_play = lv_btn_create(_bg_obj);
    lv_obj_set_size(_btn_play, 80, 80);
    lv_obj_align(_btn_play, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_obj_set_style_radius(_btn_play, 40, 0); // Circular
    lv_obj_set_style_bg_color(_btn_play, lv_color_hex(0xFFFFFF), 0);
    lv_obj_add_event_cb(_btn_play, on_btn_play_clicked, LV_EVENT_CLICKED, this);
    
    _label_play_icon = lv_label_create(_btn_play);
    lv_obj_set_style_text_font(_label_play_icon, &lv_font_montserrat_40, 0);
    lv_obj_set_style_text_color(_label_play_icon, lv_color_hex(0x000000), 0);
    lv_label_set_text(_label_play_icon, LV_SYMBOL_PLAY);
    lv_obj_center(_label_play_icon);

    // Prev Button
    _btn_prev = lv_btn_create(_bg_obj);
    lv_obj_set_size(_btn_prev, 60, 60);
    lv_obj_align_to(_btn_prev, _btn_play, LV_ALIGN_OUT_LEFT_MID, -30, 0);
    lv_obj_set_style_radius(_btn_prev, 30, 0);
    lv_obj_set_style_bg_color(_btn_prev, lv_color_hex(0x333333), 0);
    lv_obj_add_event_cb(_btn_prev, on_btn_prev_clicked, LV_EVENT_CLICKED, this);
    lv_obj_t* label_prev = lv_label_create(_btn_prev);
    lv_obj_set_style_text_font(label_prev, &lv_font_montserrat_28, 0);
    lv_label_set_text(label_prev, LV_SYMBOL_PREV);
    lv_obj_center(label_prev);

    // Next Button
    _btn_next = lv_btn_create(_bg_obj);
    lv_obj_set_size(_btn_next, 60, 60);
    lv_obj_align_to(_btn_next, _btn_play, LV_ALIGN_OUT_RIGHT_MID, 30, 0);
    lv_obj_set_style_radius(_btn_next, 30, 0);
    lv_obj_set_style_bg_color(_btn_next, lv_color_hex(0x333333), 0);
    lv_obj_add_event_cb(_btn_next, on_btn_next_clicked, LV_EVENT_CLICKED, this);
    lv_obj_t* label_next = lv_label_create(_btn_next);
    lv_obj_set_style_text_font(label_next, &lv_font_montserrat_28, 0);
    lv_label_set_text(label_next, LV_SYMBOL_NEXT);
    lv_obj_center(label_next);

    // Initial theme apply
    apply_theme(_current_source.empty() ? "UNKNOWN" : _current_source.c_str());

    return true;
}

bool AppMediaPlayer::back() {
    return true; // Use default back behavior
}

bool AppMediaPlayer::close() {
    return true; // Use default close behavior
}

void AppMediaPlayer::apply_theme(const char* source) {
    if (!_bg_obj) return;
    
    std::string src(source);
    // Convert to upper for simple comparison
    for(auto &c : src) c = toupper(c);

    lv_color_t primary_color = lv_color_hex(0x333333); // Default border/accent
    lv_color_t text_color = lv_color_hex(0xFFFFFF); // Default text
    lv_color_t bg_color = lv_color_hex(0x121212); // Default dark BG

    if (src.find("SPOTIFY") != std::string::npos) {
        primary_color = lv_color_hex(0x1DB954); // Spotify Green
        bg_color = lv_color_hex(0x1DB954); // Full green background per request
        text_color = lv_color_hex(0x000000); // Black text
    } else if (src.find("YOUTUBE") != std::string::npos) {
        primary_color = lv_color_hex(0xFF0000); // YouTube Red
        bg_color = lv_color_hex(0xFF0000); // Full red background
        text_color = lv_color_hex(0xFFFFFF); // White text
    } else {
        // Unknown or other
        bg_color = lv_color_hex(0x000000);
        primary_color = lv_color_hex(0xFFFFFF);
        text_color = lv_color_hex(0xFFFFFF);
    }

    lv_obj_set_style_bg_color(_bg_obj, bg_color, 0);
    lv_obj_set_style_border_color(_bg_obj, primary_color, 0);
    
    // Update texts
    if (_label_title) lv_obj_set_style_text_color(_label_title, text_color, 0);
    if (_label_artist) lv_obj_set_style_text_color(_label_artist, text_color, 0);
    if (_label_source) {
        lv_obj_set_style_text_color(_label_source, text_color, 0);
        lv_label_set_text(_label_source, src.c_str());
    }

    // Play button always inverted
    if (_btn_play) {
        lv_obj_set_style_bg_color(_btn_play, text_color, 0);
        lv_obj_set_style_text_color(_label_play_icon, bg_color, 0);
    }
}

void AppMediaPlayer::update_media_data(const char* source, const char* title, const char* artist, const char* state) {
    if (!_instance) return;

    _instance->_current_source = source ? source : "";
    _instance->_current_state = state ? state : "PAUSED";
    
    if (!_instance->_bg_obj) return; // App is not running/visible
    
    if (title) lv_label_set_text(_instance->_label_title, title);
    if (artist) lv_label_set_text(_instance->_label_artist, artist);
    
    _instance->apply_theme(_instance->_current_source.c_str());

    if (_instance->_current_state == "PLAYING") {
        lv_label_set_text(_instance->_label_play_icon, LV_SYMBOL_PAUSE);
    } else {
        lv_label_set_text(_instance->_label_play_icon, LV_SYMBOL_PLAY);
    }
}

void AppMediaPlayer::send_media_command(const char* cmd) {
    // Write cmd to some output mechanism.
    // For now, print to serial. In future, write back to BLE characteristic!
    ESP_UTILS_LOGI("Media Command: %s", cmd);
}

void AppMediaPlayer::on_btn_prev_clicked(lv_event_t* e) {
    AppMediaPlayer* app = (AppMediaPlayer*)lv_event_get_user_data(e);
    if(app) app->send_media_command("PREV");
}

void AppMediaPlayer::on_btn_play_clicked(lv_event_t* e) {
    AppMediaPlayer* app = (AppMediaPlayer*)lv_event_get_user_data(e);
    if(!app) return;
    
    if (app->_current_state == "PLAYING") {
        app->_current_state = "PAUSED";
        app->send_media_command("PAUSE");
        lv_label_set_text(app->_label_play_icon, LV_SYMBOL_PLAY);
    } else {
        app->_current_state = "PLAYING";
        app->send_media_command("PLAY");
        lv_label_set_text(app->_label_play_icon, LV_SYMBOL_PAUSE);
    }
}

void AppMediaPlayer::on_btn_next_clicked(lv_event_t* e) {
    AppMediaPlayer* app = (AppMediaPlayer*)lv_event_get_user_data(e);
    if(app) app->send_media_command("NEXT");
}

// C Wrapper
void app_media_player_update_from_ble(const char* payload) {
    // Payload format: SOURCE|TITLE|ARTIST|STATE
    // Example: SPOTIFY|Blinding Lights|The Weeknd|PLAYING
    
    std::string s(payload);
    std::vector<std::string> parts;
    std::stringstream ss(s);
    std::string item;
    
    while (std::getline(ss, item, '|')) {
        parts.push_back(item);
    }

    if (parts.size() >= 4) {
        AppMediaPlayer::update_media_data(parts[0].c_str(), parts[1].c_str(), parts[2].c_str(), parts[3].c_str());
    } else if (parts.size() >= 1) {
        // Just source, empty others
        AppMediaPlayer::update_media_data(parts[0].c_str(), "Unknown", "Unknown", "PAUSED");
    }
}
