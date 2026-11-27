#include "app_tars.hpp"
#include "pc_control.hpp"
#include "tv_control.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "bsp/esp-bsp.h"
#include "bsp_board_extra.h"
#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "display_manager.h"
#include "esp_codec_dev.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include "app_local_music.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>

extern "C" {
#include "ble_manager.h"
#include "wifi_manager.h"
#include "rtc_lib.h"
esp_codec_dev_handle_t bsp_get_shared_mic_codec(void);
}

extern const lv_image_dsc_t icon_voice_recorder;
extern esp_codec_dev_handle_t spk_codec_dev;

static const char* TAG = "TARS-3X";

#define ESPHOME_SALON_IP     "192.168.1.102"
#define TARS_SAMPLE_RATE     16000
#define TARS_FRAME_SAMPLES   512                                      // 32ms per frame at 16kHz
#define TARS_FRAME_BYTES     (TARS_FRAME_SAMPLES * 2)                 // 1024 bytes per 16-bit PCM frame
#define TARS_PREROLL_FRAMES  6                                        // 192ms pre-roll (preserves first Turkish syllable!)
#define TARS_MAX_REC_SEC     12                                       // 12 seconds for full long Turkish sentences
#define TARS_MAX_PCM_BYTES   (TARS_SAMPLE_RATE * 2 * TARS_MAX_REC_SEC)
#define TARS_ULAW_HDR_BYTES  58                                       // Standard RIFF/fmt(18)/fact/data header for u-law
#define TARS_MAX_ULAW_BYTES  (TARS_SAMPLE_RATE * TARS_MAX_REC_SEC)    // Half size of 16-bit PCM!
#define TARS_TEXT_CAP        512
#define TARS_HIST_TURNS      2

static AppTars* s_tars_instance = nullptr;
static TaskHandle_t s_tars_task = nullptr;
static volatile bool s_tars_app_active = false;
static volatile bool s_tars_wake_window_req = false;
static volatile bool s_tars_abort_audio = false;
static volatile bool s_tars_manual_toggle = false;
static volatile bool s_wake_pill_dismissed = false;
static char s_groq_api_key[160] = {0};

// Shared 1-Bit Pixel TARS Monolith Palettes & Primitives (used by both Full App & Sol Ust Capraz Wake Overlay)
struct TarsMonoPalette {
    const char* name;
    uint32_t fg;
    uint32_t mid;
    uint32_t dim;
    uint32_t acc;
};

static const TarsMonoPalette kTarsPalettes[4] = {
    { "BEYAZ", 0xF1F5F9, 0x64748B, 0x1E293B, 0xFB923C }, // 0: Titanyum Beyaz (default)
    { "CELIK", 0xE2E8F0, 0x475569, 0x0F172A, 0x38BDF8 }, // 1: Endurance Buz Celik
    { "AMBER", 0xFDBA74, 0x9A3412, 0x271306, 0xFEF08A }, // 2: Flipper Turuncu
    { "YESIL", 0x86EFAC, 0x166534, 0x052E16, 0xFACC15 }, // 3: Fosfor Yesil
};

static const TarsMonoPalette kTarsTurboPal  = { "TURBO", 0xF87171, 0x991B1B, 0x2A090A, 0xFDE047 };
static const TarsMonoPalette kTarsLowBatPal = { "UYKU",  0xD97706, 0x78350F, 0x1C1005, 0xF59E0B };
static const char* kTarsStyleNames[3] = { "SAF", "NASA", "ASIM" };

static int s_shared_ui_tone = 0;
static int s_shared_ui_style = 0;
static bool s_shared_prefs_loaded = false;

static void tars_ensure_shared_prefs(void) {
    if (s_shared_prefs_loaded) return;
    s_shared_prefs_loaded = true;
    nvs_handle_t h;
    if (nvs_open("tars_cfg", NVS_READONLY, &h) == ESP_OK) {
        int32_t v = 0;
        if (nvs_get_i32(h, "ui_tone", &v) == ESP_OK && v >= 0 && v < 4) s_shared_ui_tone = (int)v;
        if (nvs_get_i32(h, "ui_style", &v) == ESP_OK && v >= 0 && v < 3) s_shared_ui_style = (int)v;
        nvs_close(h);
    }
}

static inline uint16_t tars_hex565(uint32_t hex) {
    uint16_t r = (uint16_t)((hex >> 16) & 0xFF);
    uint16_t g = (uint16_t)((hex >> 8) & 0xFF);
    uint16_t b = (uint16_t)(hex & 0xFF);
    return (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}

static inline void mono_px_s(uint16_t* buf, int buf_w, int buf_h, int p, int gx, int gy, int gw, int gh, uint16_t col) {
    int x0 = gx * p;
    int y0 = gy * p;
    int x1 = (gx + gw) * p;
    int y1 = (gy + gh) * p;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > buf_w) x1 = buf_w;
    if (y1 > buf_h) y1 = buf_h;
    for (int y = y0; y < y1; y++) {
        uint16_t* row = buf + (y * buf_w);
        for (int x = x0; x < x1; x++) {
            row[x] = col;
        }
    }
}

static void mono_draw_tars_word_s(uint16_t* buf, int buf_w, int buf_h, int p, int gx, int gy, uint16_t col) {
    static const uint8_t glyphs[4][5] = {
        { 0b111, 0b010, 0b010, 0b010, 0b010 }, // T
        { 0b010, 0b101, 0b111, 0b101, 0b101 }, // A
        { 0b110, 0b101, 0b110, 0b101, 0b101 }, // R
        { 0b011, 0b100, 0b010, 0b001, 0b110 }, // S
    };
    for (int ch = 0; ch < 4; ch++) {
        int cx = gx + ch * 4;
        for (int r = 0; r < 5; r++) {
            uint8_t bits = glyphs[ch][r];
            if (bits & 0b100) mono_px_s(buf, buf_w, buf_h, p, cx + 0, gy + r, 1, 1, col);
            if (bits & 0b010) mono_px_s(buf, buf_w, buf_h, p, cx + 1, gy + r, 1, 1, col);
            if (bits & 0b001) mono_px_s(buf, buf_w, buf_h, p, cx + 2, gy + r, 1, 1, col);
        }
    }
}

// Sol Ust Capraz (Top-Left Diagonal) Animated 1-Bit Pixel TARS Wake Overlay (24 KB PSRAM sprite!)
static constexpr int WAKE_MONO_GW = 35;
static constexpr int WAKE_MONO_GH = 22;
static constexpr int WAKE_MONO_P  = 4;
static constexpr int WAKE_MONO_W  = WAKE_MONO_GW * WAKE_MONO_P; // 140
static constexpr int WAKE_MONO_H  = WAKE_MONO_GH * WAKE_MONO_P; // 88

static uint16_t* s_wake_robot_buf = nullptr;
static lv_image_dsc_t s_wake_robot_dsc = {
    .header = {
        .magic = LV_IMAGE_HEADER_MAGIC,
        .cf = LV_COLOR_FORMAT_RGB565,
        .flags = 0,
        .w = WAKE_MONO_W,
        .h = WAKE_MONO_H,
        .stride = WAKE_MONO_W * 2,
        .reserved_2 = 0
    },
    .data_size = WAKE_MONO_W * WAKE_MONO_H * 2,
    .data = nullptr,
    .reserved = NULL
};

static lv_obj_t* s_wake_pill = nullptr;       // Transparent top-left animated container
static lv_obj_t* s_wake_robot_img = nullptr;  // 140x88 live 1-bit pixel TARS robot
static lv_obj_t* s_wake_card = nullptr;       // Right-hand 1-bit terminal speech bubble
static lv_obj_t* s_wake_pill_hdr = nullptr;
static lv_obj_t* s_wake_pill_lbl = nullptr;
static lv_obj_t* s_wake_prog_bar = nullptr;
static lv_timer_t* s_wake_anim_timer = nullptr;
static volatile bool s_wake_visible_target = false;
static volatile int s_wake_vis_state = TARS_VIS_LISTEN;
static volatile int s_wake_mic_pct = 0;
static volatile bool s_wake_countdown_active = false;
static volatile int64_t s_wake_listen_start_us = 0;

// Short-term conversation memory (stored in PSRAM) for multi-turn Turkish context
static char* s_hist_user[TARS_HIST_TURNS] = {nullptr, nullptr};
static char* s_hist_asst[TARS_HIST_TURNS] = {nullptr, nullptr};
static int s_hist_count = 0;

// Persistent Keep-Alive Groq TLS Client (eliminates ~600ms TLS handshake on warm turns!)
static esp_http_client_handle_t s_groq_client = nullptr;
static bool s_groq_warm = false;
static int64_t s_groq_last_use_us = 0;

static void tars_push_history(const char* user_utf8, const char* asst_utf8) {
    if (!user_utf8 || !asst_utf8 || strlen(user_utf8) < 2 || strlen(asst_utf8) < 2) return;
    for (int i = 0; i < TARS_HIST_TURNS; i++) {
        if (!s_hist_user[i]) s_hist_user[i] = (char*)heap_caps_calloc(1, TARS_TEXT_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_hist_asst[i]) s_hist_asst[i] = (char*)heap_caps_calloc(1, TARS_TEXT_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_hist_user[0] || !s_hist_asst[0] || !s_hist_user[1] || !s_hist_asst[1]) return;

    if (s_hist_count >= TARS_HIST_TURNS) {
        strncpy(s_hist_user[0], s_hist_user[1], TARS_TEXT_CAP - 1);
        strncpy(s_hist_asst[0], s_hist_asst[1], TARS_TEXT_CAP - 1);
        s_hist_count = TARS_HIST_TURNS - 1;
    }
    strncpy(s_hist_user[s_hist_count], user_utf8, TARS_TEXT_CAP - 1);
    s_hist_user[s_hist_count][TARS_TEXT_CAP - 1] = '\0';
    strncpy(s_hist_asst[s_hist_count], asst_utf8, TARS_TEXT_CAP - 1);
    s_hist_asst[s_hist_count][TARS_TEXT_CAP - 1] = '\0';
    s_hist_count++;
}

// Normalize Turkish UTF-8 & smart typography to clean ASCII so lv_font_montserrat renders cleanly without missing glyphs
static void tars_utf8_to_ascii(char* dst, const char* src, size_t max_len) {
    if (!dst || max_len == 0) return;
    if (!src) { dst[0] = '\0'; return; }

    size_t i = 0, o = 0;
    while (src[i] && o + 1 < max_len) {
        unsigned char c = (unsigned char)src[i];
        if (c == 0xC3 && src[i + 1]) {
            unsigned char c2 = (unsigned char)src[i + 1];
            if (c2 == 0xA7) { dst[o++] = 'c'; i += 2; continue; } // ç
            if (c2 == 0x87) { dst[o++] = 'C'; i += 2; continue; } // Ç
            if (c2 == 0xB6) { dst[o++] = 'o'; i += 2; continue; } // ö
            if (c2 == 0x96) { dst[o++] = 'O'; i += 2; continue; } // Ö
            if (c2 == 0xBC) { dst[o++] = 'u'; i += 2; continue; } // ü
            if (c2 == 0x9C) { dst[o++] = 'U'; i += 2; continue; } // Ü
            if (c2 == 0xA2) { dst[o++] = 'a'; i += 2; continue; } // â
            if (c2 == 0x82) { dst[o++] = 'A'; i += 2; continue; } // Â
            if (c2 == 0xAE) { dst[o++] = 'i'; i += 2; continue; } // î
            if (c2 == 0x8E) { dst[o++] = 'I'; i += 2; continue; } // Î
            if (c2 == 0xBB) { dst[o++] = 'u'; i += 2; continue; } // û
            if (c2 == 0x9B) { dst[o++] = 'U'; i += 2; continue; } // Û
            i += 2;
            continue;
        } else if (c == 0xC4 && src[i + 1]) {
            unsigned char c2 = (unsigned char)src[i + 1];
            if (c2 == 0x9F) { dst[o++] = 'g'; i += 2; continue; } // ğ
            if (c2 == 0x9E) { dst[o++] = 'G'; i += 2; continue; } // Ğ
            if (c2 == 0xB1) { dst[o++] = 'i'; i += 2; continue; } // ı
            if (c2 == 0xB0) { dst[o++] = 'I'; i += 2; continue; } // İ
            i += 2;
            continue;
        } else if (c == 0xC5 && src[i + 1]) {
            unsigned char c2 = (unsigned char)src[i + 1];
            if (c2 == 0x9F) { dst[o++] = 's'; i += 2; continue; } // ş
            if (c2 == 0x9E) { dst[o++] = 'S'; i += 2; continue; } // Ş
            i += 2;
            continue;
        } else if (c == 0xE2 && src[i + 1] && src[i + 2]) {
            unsigned char c2 = (unsigned char)src[i + 1];
            unsigned char c3 = (unsigned char)src[i + 2];
            if (c2 == 0x80 && (c3 == 0x98 || c3 == 0x99)) { dst[o++] = '\''; i += 3; continue; }
            if (c2 == 0x80 && (c3 == 0x9C || c3 == 0x9D)) { dst[o++] = '"';  i += 3; continue; }
            if (c2 == 0x80 && (c3 == 0x93 || c3 == 0x94)) { dst[o++] = '-';  i += 3; continue; }
            if (c2 == 0x80 && c3 == 0xA6) {
                for (int k = 0; k < 3 && o + 1 < max_len; k++) dst[o++] = '.';
                i += 3;
                continue;
            }
            i += 3;
            continue;
        } else if (c >= 0xF0) {
            i += (src[i + 1] && src[i + 2] && src[i + 3]) ? 4 : 1;
            continue;
        } else if (c >= 0xE0) {
            i += (src[i + 1] && src[i + 2]) ? 3 : 1;
            continue;
        } else if (c >= 0xC0) {
            i += src[i + 1] ? 2 : 1;
            continue;
        } else if (c >= 0x80) {
            i++;
            continue;
        }
        dst[o++] = (char)c;
        i++;
    }
    dst[o] = '\0';
}

// Detect known Whisper Turkish silence/noise hallucination phrases
static bool tars_is_whisper_hallucination(const char* ascii_text) {
    if (!ascii_text) return true;
    char low[256] = {0};
    size_t j = 0;
    for (size_t i = 0; ascii_text[i] && j < sizeof(low) - 1; i++) {
        unsigned char c = (unsigned char)ascii_text[i];
        if (c == '\r' || c == '\n') continue;
        low[j++] = (char)tolower(c);
    }
    char* p = low;
    while (*p == ' ' || *p == '.' || *p == ',' || *p == '!' || *p == '?') p++;
    size_t len = strlen(p);
    while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '.' || p[len - 1] == ',' || p[len - 1] == '!' || p[len - 1] == '?')) {
        p[--len] = '\0';
    }
    if (len < 2) return true;

    const char* bad[] = {
        "altyazi", "m.k", "izlediginiz icin", "abone olmayi", "abone olun",
        "kanalima", "begenmeyi unutmayin", "tesekkur ederim", "tesekkurler",
        "devam edecek", "sonraki videoda", "gorusmek uzere", "bye", "thank you"
    };
    for (size_t k = 0; k < sizeof(bad) / sizeof(bad[0]); k++) {
        if (strstr(p, bad[k]) != nullptr) {
            if (k >= 7 && len > 24) continue;
            return true;
        }
    }
    return false;
}

// Load/Save Groq API Key from NVS or SD Card
static void tars_load_api_key(void) {
    if (strlen(s_groq_api_key) > 10) return;

    nvs_handle_t my_handle;
    if (nvs_open("tars_cfg", NVS_READONLY, &my_handle) == ESP_OK) {
        size_t req_size = sizeof(s_groq_api_key);
        if (nvs_get_str(my_handle, "api_key", s_groq_api_key, &req_size) == ESP_OK) {
            ESP_LOGI(TAG, "Loaded TARS API key from NVS (%d chars)", (int)strlen(s_groq_api_key));
        }
        nvs_close(my_handle);
    }

    if (strlen(s_groq_api_key) < 10) {
        FILE* f = fopen("/sdcard/tars_key.txt", "r");
        if (f) {
            if (fgets(s_groq_api_key, sizeof(s_groq_api_key) - 1, f)) {
                size_t l = strlen(s_groq_api_key);
                while (l > 0 && (s_groq_api_key[l - 1] == '\r' || s_groq_api_key[l - 1] == '\n' || s_groq_api_key[l - 1] == ' ')) {
                    s_groq_api_key[--l] = '\0';
                }
                ESP_LOGI(TAG, "Loaded TARS API key from /sdcard/tars_key.txt");
            }
            fclose(f);
        }
    }
    if (strlen(s_groq_api_key) < 10) {
        /* Provide Groq API key via Companion BLE sync, NVS, or /sdcard/tars_key.txt */
    }
}

extern "C" void app_tars_set_api_key_from_ble(const char* key) {
    if (!key || strlen(key) < 8) return;
    strncpy(s_groq_api_key, key, sizeof(s_groq_api_key) - 1);
    s_groq_api_key[sizeof(s_groq_api_key) - 1] = '\0';

    nvs_handle_t my_handle;
    if (nvs_open("tars_cfg", NVS_READWRITE, &my_handle) == ESP_OK) {
        nvs_set_str(my_handle, "api_key", s_groq_api_key);
        nvs_commit(my_handle);
        nvs_close(my_handle);
    }
    FILE* f = fopen("/sdcard/tars_key.txt", "w");
    if (f) {
        fprintf(f, "%s", s_groq_api_key);
        fclose(f);
    }
    ESP_LOGI(TAG, "Saved new TARS API key to NVS & SD card!");
    bsp_vibrate_pulse(80);
}

extern "C" void app_tars_stop_all_audio(void) {
    s_tars_abort_audio = true;
    s_tars_wake_window_req = false;
    s_wake_pill_dismissed = true;
}

static char s_live_pc_telemetry[160] = {0};

// Deterministic Firmware Safety Gate: blocks LLM-hallucinated relay/power actions on casual chat!
// (e.g. prevents "Merhaba" or "Bana fikra anlat" from EVER toggling lights or shutting down PC!)
static bool tars_is_action_authorized(const char* p, const char* user_transcript_ascii) {
    if (!user_transcript_ascii || user_transcript_ascii[0] == '\0') {
        return true; // Direct BLE command override (no voice transcript)
    }
    char low[256] = {0};
    for (size_t i = 0; user_transcript_ascii[i] && i < sizeof(low) - 1; i++) {
        low[i] = (char)tolower((unsigned char)user_transcript_ascii[i]);
    }

    if (strcmp(p, "SALON_ON") == 0 || strcmp(p, "SALON_OFF") == 0 || strcmp(p, "SALON_TOGGLE") == 0) {
        // NOTE: Include BOTH "isik" and "isig" (Turkish consonant mutation: "isigi ac/kapat")!
        bool ok = (strstr(low, "isik") || strstr(low, "isig") || strstr(low, "lamba") || strstr(low, "salon") ||
                   strstr(low, "aydin") || strstr(low, "karanl") || strstr(low, "ampul") ||
                   strstr(low, "avize") || strstr(low, "spot") || strstr(low, "sinema") ||
                   strstr(low, "mola") || strstr(low, "oda") || strstr(low, "yak") || strstr(low, "sondur"));
        if (!ok) {
            ESP_LOGW(TAG, "SAFETY GATE: Blocked unsolicited light action [%s] for speech '%s'", p, low);
        }
        return ok;
    }

    if (strncmp(p, "PC_SHUTDOWN", 11) == 0 || strcmp(p, "PC_SLEEP") == 0 ||
        strcmp(p, "PC_WAKE") == 0 || strcmp(p, "PC_LOCK") == 0 || strcmp(p, "PC_SCREEN_OFF") == 0) {
        bool ok = (strstr(low, "bilgisayar") || strstr(low, "pc") || strstr(low, "kasa") ||
                   strstr(low, "sistem") || strstr(low, "ekran") || strstr(low, "monitor") ||
                   strstr(low, "kapat") || strstr(low, "uyut") || strstr(low, "uyku") ||
                   strstr(low, "kilit") || strstr(low, "uyandir") || strstr(low, "iptal") || strstr(low, "ac"));
        if (!ok) {
            ESP_LOGW(TAG, "SAFETY GATE: Blocked unsolicited PC power action [%s] for speech '%s'", p, low);
        }
        return ok;
    }

    if (strncmp(p, "TV_", 3) == 0) {
        bool ok = (strstr(low, "tv") || strstr(low, "televizyon") || strstr(low, "tivibu") ||
                   strstr(low, "kanal") || strstr(low, "youtube") || strstr(low, "netflix") ||
                   strstr(low, "spotify") || strstr(low, "prime") || strstr(low, "disney") ||
                   strstr(low, "ses") || strstr(low, "durdur") || strstr(low, "oynat") ||
                   strstr(low, "devam") || strstr(low, "sar") || strstr(low, "geri") ||
                   strstr(low, "ileri") || strstr(low, "kumanda") || strstr(low, "film") ||
                   strstr(low, "dizi") || strstr(low, "ac") || strstr(low, "kapat"));
        if (!ok) {
            ESP_LOGW(TAG, "SAFETY GATE: Blocked unsolicited TV action [%s] for speech '%s'", p, low);
        }
        return ok;
    }

    return true;
}

enum TarsActionOutcome {
    TARS_ACT_NONE = 0,
    TARS_ACT_OK = 1,
    TARS_ACT_PC_UNREACHABLE = 2,
    TARS_ACT_TV_UNREACHABLE = 3,
};

// Execute a single action command honestly (never claims success if PC/TV is unreachable!)
static TarsActionOutcome tars_execute_single_action(const char* raw_item, const char* user_transcript_ascii,
                                                    char* out_badge, size_t max_badge) {
    if (!raw_item) return TARS_ACT_NONE;
    char cmd[220];
    cmd[0] = '\0';
    const char* s = raw_item;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r' || s[len - 1] == '\n')) len--;
    if (len == 0) return TARS_ACT_NONE;
    if (len >= sizeof(cmd)) len = sizeof(cmd) - 1;
    memcpy(cmd, s, len);
    cmd[len] = '\0';

    const char* p = cmd;
    if (!tars_is_action_authorized(p, user_transcript_ascii)) {
        return TARS_ACT_NONE;
    }
    ESP_LOGI(TAG, "ACTION DISPATCHED: [%s]", p);

    if (strcmp(p, "SALON_ON") == 0) {
        pc_control_esphome("/switch/salon_isigi/turn_on");
        strncpy(out_badge, "SALON ISIGI ACILDI", max_badge - 1);
    } else if (strcmp(p, "SALON_OFF") == 0) {
        pc_control_esphome("/switch/salon_isigi/turn_off");
        strncpy(out_badge, "SALON ISIGI KAPANDI", max_badge - 1);
    } else if (strcmp(p, "SALON_TOGGLE") == 0) {
        pc_control_esphome("/switch/salon_isigi/toggle");
        strncpy(out_badge, "SALON ISIGI TETIKLENDI", max_badge - 1);
    } else if (strcmp(p, "PC_WAKE") == 0) {
        pc_control_wake_lan();
        strncpy(out_badge, "PC UYANDIRILDI (WoL)", max_badge - 1);
    } else if (strcmp(p, "PC_GET_STATS") == 0) {
        if (pc_control_get_stats(s_live_pc_telemetry, sizeof(s_live_pc_telemetry))) {
            snprintf(out_badge, max_badge - 1, "PC: %.48s", s_live_pc_telemetry);
        } else {
            s_live_pc_telemetry[0] = '\0';
            strncpy(out_badge, "PC ERISILEMEDI", max_badge - 1);
            out_badge[max_badge - 1] = '\0';
            return TARS_ACT_PC_UNREACHABLE;
        }
    } else if (strcmp(p, "PC_MODE_BREAK") == 0) {
        bool ok = pc_control_send("PC_MODE_BREAK");
        pc_control_esphome("/switch/salon_isigi/turn_on");
        strncpy(out_badge, ok ? "MOD: MUTFAK MOLASI" : "ISIK OK / PC ERISILEMEDI", max_badge - 1);
    } else if (strcmp(p, "PC_MODE_MOVIE") == 0) {
        bool ok = pc_control_send("PC_MODE_MOVIE");
        pc_control_esphome("/switch/salon_isigi/turn_off");
        strncpy(out_badge, ok ? "MOD: SINEMA" : "ISIK OK / PC ERISILEMEDI", max_badge - 1);
    } else if (strcmp(p, "PC_MODE_STUDY") == 0) {
        if (!pc_control_send("PC_MODE_STUDY")) {
            strncpy(out_badge, "PC ERISILEMEDI", max_badge - 1);
            out_badge[max_badge - 1] = '\0';
            return TARS_ACT_PC_UNREACHABLE;
        }
        strncpy(out_badge, "MOD: DERS / ODAK", max_badge - 1);
    } else if (strcmp(p, "PC_MODE_GAME") == 0) {
        if (s_tars_instance) s_tars_instance->set_turbo_mode(true);
        bool ok = pc_control_send("PC_MODE_GAME");
        strncpy(out_badge, ok ? "MOD: OYUN (PC + TURBO)" : "TURBO AKTIF (PC KAPALI)", max_badge - 1);
    } else if (strcmp(p, "TARS_TURBO_ON") == 0) {
        if (s_tars_instance) s_tars_instance->set_turbo_mode(true);
        strncpy(out_badge, "TARS TURBO KIRMIZI AKTIF", max_badge - 1);
    } else if (strcmp(p, "TARS_TURBO_OFF") == 0) {
        if (s_tars_instance) s_tars_instance->set_turbo_mode(false);
        strncpy(out_badge, "TARS NORMAL MOD", max_badge - 1);
    } else if (strncmp(p, "PC_", 3) == 0) {
        bool ok = pc_control_send(p);
        if (!ok) {
            snprintf(out_badge, max_badge - 1, "PC ERISILEMEDI (%s)", pc_control_get_cached_ip());
            out_badge[max_badge - 1] = '\0';
            return TARS_ACT_PC_UNREACHABLE;
        }
        if (strcmp(p, "PC_SLEEP") == 0) strncpy(out_badge, "PC UYKUYA ALINDI", max_badge - 1);
        else if (strcmp(p, "PC_LOCK") == 0) strncpy(out_badge, "PC KILITLENDI", max_badge - 1);
        else if (strcmp(p, "PC_SCREEN_OFF") == 0) strncpy(out_badge, "MONITOR KAPATILDI", max_badge - 1);
        else if (strcmp(p, "PC_SHUTDOWN_CANCEL") == 0) strncpy(out_badge, "KAPANMA IPTAL", max_badge - 1);
        else if (strncmp(p, "PC_SHUTDOWN", 11) == 0) strncpy(out_badge, "PC KAPANMA KURULDU", max_badge - 1);
        else if (strstr(p, "PC_MEDIA_PLAY_PAUSE")) strncpy(out_badge, "PC MEDYA DUR/OYNAT", max_badge - 1);
        else if (strstr(p, "PC_MEDIA_NEXT")) strncpy(out_badge, "SONRAKI PARCA", max_badge - 1);
        else if (strstr(p, "PC_MEDIA_PREV")) strncpy(out_badge, "ONCEKI PARCA", max_badge - 1);
        else if (strstr(p, "PC_VOL_UP")) strncpy(out_badge, "PC SES +", max_badge - 1);
        else if (strstr(p, "PC_VOL_DOWN")) strncpy(out_badge, "PC SES -", max_badge - 1);
        else if (strstr(p, "PC_VOL_MUTE")) strncpy(out_badge, "PC SES SUSTURULDU", max_badge - 1);
        else if (strstr(p, "PC_VOL_SET:")) snprintf(out_badge, max_badge - 1, "PC SES %%%d", atoi(p + 11));
        else if (strstr(p, "PC_GOOGLE_SEARCH:")) strncpy(out_badge, "GOOGLE ARANDI", max_badge - 1);
        else if (strstr(p, "PC_OPEN_URL:")) strncpy(out_badge, "SITE ACILDI", max_badge - 1);
        else if (strstr(p, "PC_YOUTUBE_PLAY:")) strncpy(out_badge, "YOUTUBE OYNATILIYOR", max_badge - 1);
        else if (strstr(p, "PC_YOUTUBE_SEARCH:")) strncpy(out_badge, "YOUTUBE ARANDI", max_badge - 1);
        else if (strstr(p, "PC_SPOTIFY_SEARCH:")) strncpy(out_badge, "SPOTIFY ARANDI", max_badge - 1);
        else if (strstr(p, "PC_TYPE_TEXT:")) strncpy(out_badge, "PC'YE YAZILDI", max_badge - 1);
        else if (strstr(p, "PC_OPEN_CLIPBOARD_URL")) strncpy(out_badge, "PANO LINKI ACILDI", max_badge - 1);
        else if (strstr(p, "PC_OPEN_APP:")) snprintf(out_badge, max_badge - 1, "ACILDI: %.32s", p + 12);
        else if (strstr(p, "PC_CLOSE_APP:")) snprintf(out_badge, max_badge - 1, "KAPATILDI: %.32s", p + 13);
        else if (strstr(p, "PC_OPEN_SPOTIFY")) strncpy(out_badge, "SPOTIFY ACILDI", max_badge - 1);
        else if (strstr(p, "PC_OPEN_YOUTUBE")) strncpy(out_badge, "YOUTUBE ACILDI", max_badge - 1);
        else if (strstr(p, "PC_OPEN_CHROME")) strncpy(out_badge, "CHROME ACILDI", max_badge - 1);
        else if (strstr(p, "PC_OPEN_DISCORD")) strncpy(out_badge, "DISCORD ACILDI", max_badge - 1);
        else if (strstr(p, "PC_OPEN_STEAM")) strncpy(out_badge, "STEAM ACILDI", max_badge - 1);
        else if (strstr(p, "PC_OPEN_VSCODE")) strncpy(out_badge, "VS CODE ACILDI", max_badge - 1);
        else if (strstr(p, "PC_SEEK_FWD")) strncpy(out_badge, "VIDEO +10SN", max_badge - 1);
        else if (strstr(p, "PC_SEEK_BACK")) strncpy(out_badge, "VIDEO -10SN", max_badge - 1);
        else if (strstr(p, "PC_CAPTIONS")) strncpy(out_badge, "ALTYAZI", max_badge - 1);
        else if (strstr(p, "PC_THEATER")) strncpy(out_badge, "SINEMA GORUNUMU", max_badge - 1);
        else if (strstr(p, "PC_SPEED_UP")) strncpy(out_badge, "VIDEO HIZLANDI", max_badge - 1);
        else if (strstr(p, "PC_SPEED_DOWN")) strncpy(out_badge, "VIDEO YAVASLADI", max_badge - 1);
        else if (strstr(p, "PC_NEW_TAB")) strncpy(out_badge, "YENI SEKME", max_badge - 1);
        else if (strstr(p, "PC_REOPEN_TAB")) strncpy(out_badge, "SEKME GERI ALINDI", max_badge - 1);
        else if (strstr(p, "PC_NEXT_TAB")) strncpy(out_badge, "SONRAKI SEKME", max_badge - 1);
        else if (strstr(p, "PC_PREV_TAB")) strncpy(out_badge, "ONCEKI SEKME", max_badge - 1);
        else if (strstr(p, "PC_CLOSE_TAB")) strncpy(out_badge, "SEKME KAPATILDI", max_badge - 1);
        else if (strstr(p, "PC_REFRESH")) strncpy(out_badge, "SAYFA YENILENDI", max_badge - 1);
        else if (strstr(p, "PC_SCROLL_DOWN")) strncpy(out_badge, "ASAGI KAYDIRILDI", max_badge - 1);
        else if (strstr(p, "PC_SCROLL_UP")) strncpy(out_badge, "YUKARI KAYDIRILDI", max_badge - 1);
        else if (strstr(p, "PC_ALT_TAB")) strncpy(out_badge, "PENCERE DEGISTI", max_badge - 1);
        else if (strstr(p, "PC_CLOSE_WINDOW")) strncpy(out_badge, "PENCERE KAPATILDI", max_badge - 1);
        else if (strstr(p, "PC_SNAP_LEFT")) strncpy(out_badge, "SOLA YASLANDI", max_badge - 1);
        else if (strstr(p, "PC_SNAP_RIGHT")) strncpy(out_badge, "SAGA YASLANDI", max_badge - 1);
        else if (strstr(p, "PC_BOSS_KEY")) strncpy(out_badge, "PANIK MODU", max_badge - 1);
        else if (strstr(p, "PC_MINIMIZE_ALL")) strncpy(out_badge, "MASAUSTU", max_badge - 1);
        else if (strstr(p, "PC_FULLSCREEN")) strncpy(out_badge, "TAM EKRAN", max_badge - 1);
        else if (strstr(p, "PC_SCREENSHOT")) strncpy(out_badge, "EKRAN GORUNTUSU", max_badge - 1);
        else strncpy(out_badge, "PC KOMUTU OK", max_badge - 1);
    } else if (strncmp(p, "TV_", 3) == 0) {
        bool ok = tv_control_send(p);
        if (!ok) {
            strncpy(out_badge, "TV ERISILEMEDI", max_badge - 1);
            out_badge[max_badge - 1] = '\0';
            return TARS_ACT_TV_UNREACHABLE;
        }
        if (strcmp(p, "TV_POWER") == 0) strncpy(out_badge, "TV GUC", max_badge - 1);
        else if (strcmp(p, "TV_SLEEP") == 0) strncpy(out_badge, "TV KAPATILDI", max_badge - 1);
        else if (strcmp(p, "TV_WAKE") == 0) strncpy(out_badge, "TV ACILDI", max_badge - 1);
        else if (strcmp(p, "TV_PAUSE") == 0) strncpy(out_badge, "TV DURAKLATILDI", max_badge - 1);
        else if (strcmp(p, "TV_PLAY") == 0) strncpy(out_badge, "TV OYNATILIYOR", max_badge - 1);
        else if (strcmp(p, "TV_PLAY_PAUSE") == 0) strncpy(out_badge, "TV OYNAT/DUR", max_badge - 1);
        else if (strcmp(p, "TV_VOL_UP") == 0 || strcmp(p, "TV_VOL_UP_1") == 0) strncpy(out_badge, "TV SES +", max_badge - 1);
        else if (strcmp(p, "TV_VOL_DOWN") == 0 || strcmp(p, "TV_VOL_DOWN_1") == 0) strncpy(out_badge, "TV SES -", max_badge - 1);
        else if (strcmp(p, "TV_MUTE") == 0) strncpy(out_badge, "TV SESSIZ", max_badge - 1);
        else if (strcmp(p, "TV_CH_UP") == 0) strncpy(out_badge, "TV SONRAKI KANAL", max_badge - 1);
        else if (strcmp(p, "TV_CH_DOWN") == 0) strncpy(out_badge, "TV ONCEKI KANAL", max_badge - 1);
        else if (strcmp(p, "TV_FWD") == 0) strncpy(out_badge, "TV ILERI SAR", max_badge - 1);
        else if (strcmp(p, "TV_REW") == 0) strncpy(out_badge, "TV GERI SAR", max_badge - 1);
        else if (strcmp(p, "TV_NEXT") == 0) strncpy(out_badge, "TV SONRAKI", max_badge - 1);
        else if (strcmp(p, "TV_PREV") == 0) strncpy(out_badge, "TV ONCEKI", max_badge - 1);
        else if (strcmp(p, "TV_HOME") == 0) strncpy(out_badge, "TV ANA EKRAN", max_badge - 1);
        else if (strcmp(p, "TV_BACK") == 0) strncpy(out_badge, "TV GERI", max_badge - 1);
        else if (strcmp(p, "TV_OK") == 0) strncpy(out_badge, "TV OK", max_badge - 1);
        else if (strcmp(p, "TV_OPEN_YOUTUBE") == 0) strncpy(out_badge, "TV YOUTUBE ACILDI", max_badge - 1);
        else if (strcmp(p, "TV_OPEN_NETFLIX") == 0) strncpy(out_badge, "TV NETFLIX ACILDI", max_badge - 1);
        else if (strcmp(p, "TV_OPEN_SPOTIFY") == 0) strncpy(out_badge, "TV SPOTIFY ACILDI", max_badge - 1);
        else if (strcmp(p, "TV_OPEN_PRIME") == 0) strncpy(out_badge, "TV PRIME VIDEO", max_badge - 1);
        else if (strcmp(p, "TV_OPEN_DISNEY") == 0) strncpy(out_badge, "TV DISNEY+", max_badge - 1);
        else if (strncmp(p, "TV_YOUTUBE_PLAY:", 16) == 0) strncpy(out_badge, "TV YOUTUBE OYNAT", max_badge - 1);
        else strncpy(out_badge, "TV KOMUTU OK", max_badge - 1);
    } else if (strncmp(p, "HUMOR_SET:", 10) == 0) {
        int new_h = atoi(p + 10);
        if (new_h >= 0 && new_h <= 100 && s_tars_instance) {
            s_tars_instance->set_humor(new_h);
        }
        snprintf(out_badge, max_badge - 1, "MIZAH %%%d YAPILDI", new_h);
    } else {
        return TARS_ACT_NONE;
    }
    out_badge[max_badge - 1] = '\0';
    return TARS_ACT_OK;
}

// Execute 1 or MULTIPLE chained actions separated by ';' (tracks PC/TV reachability honestly!)
static bool tars_fire_action_tag(const char* tag_inner, const char* user_transcript_ascii,
                                 char* out_action_badge, size_t max_badge,
                                 bool* out_pc_unreachable = nullptr,
                                 bool* out_tv_unreachable = nullptr) {
    if (out_pc_unreachable) *out_pc_unreachable = false;
    if (out_tv_unreachable) *out_tv_unreachable = false;
    if (!tag_inner || tag_inner[0] == '\0') return false;

    char buf[256];
    strncpy(buf, tag_inner, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    int executed = 0;
    char combined[96];
    combined[0] = '\0';
    char* cur = buf;

    while (cur && *cur != '\0' && executed < 5) {
        char* sep = strchr(cur, ';');
        if (sep) *sep = '\0';

        char one_badge[64] = {0};
        TarsActionOutcome outcome = tars_execute_single_action(cur, user_transcript_ascii, one_badge, sizeof(one_badge));
        if (outcome != TARS_ACT_NONE) {
            if (outcome == TARS_ACT_PC_UNREACHABLE && out_pc_unreachable) *out_pc_unreachable = true;
            if (outcome == TARS_ACT_TV_UNREACHABLE && out_tv_unreachable) *out_tv_unreachable = true;
            if (executed > 0 && strlen(combined) + 3 < sizeof(combined)) {
                strncat(combined, " + ", sizeof(combined) - strlen(combined) - 1);
            }
            if (strlen(combined) + 2 < sizeof(combined)) {
                strncat(combined, one_badge, sizeof(combined) - strlen(combined) - 1);
            }
            executed++;
            if (sep) vTaskDelay(pdMS_TO_TICKS(60));
        }
        cur = sep ? (sep + 1) : nullptr;
    }

    if (executed == 0) return false;
    if (executed == 1) {
        snprintf(out_action_badge, max_badge - 1, "EYLEM: %.50s", combined);
    } else {
        snprintf(out_action_badge, max_badge - 1, "ZINCIR (%d): %.38s", executed, combined);
    }
    out_action_badge[max_badge - 1] = '\0';
    return true;
}

// Play TARS signature acoustic telemetry cadence on watch speaker (ES8311)
static void tars_play_acoustic_response(bool is_action) {
    if (s_tars_abort_audio || !spk_codec_dev) return;
    if (!bsp_audio_lock(350)) return;

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 0,
        .sample_rate = 16000,
        .mclk_multiple = 256,
    };

    const int N = 960; // 60ms tone pulses at 16kHz
    int16_t* tone = (int16_t*)heap_caps_malloc(N * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!tone) tone = (int16_t*)malloc(N * sizeof(int16_t));
    if (!tone) {
        bsp_audio_unlock();
        return;
    }

    bsp_audio_power_amp_enable(true);
    if (esp_codec_dev_open(spk_codec_dev, &fs) == ESP_OK) {
        esp_codec_dev_set_out_vol(spk_codec_dev, 65);
        float freqs[3] = { is_action ? 587.3f : 523.2f, is_action ? 880.0f : 659.2f, is_action ? 1174.6f : 784.0f };
        for (int k = 0; k < (is_action ? 3 : 2) && !s_tars_abort_audio; k++) {
            for (int i = 0; i < N; i++) {
                float env = 1.0f - ((float)i / (float)N) * 0.5f;
                float s = sinf(2.0f * (float)M_PI * freqs[k] * ((float)i / 16000.0f));
                tone[i] = (int16_t)(s * 16000.0f * env);
            }
            esp_codec_dev_write(spk_codec_dev, tone, N * sizeof(int16_t));
        }
        esp_codec_dev_close(spk_codec_dev);
    }
    free(tone);
    bsp_audio_power_amp_enable(false);
    bsp_audio_standby();
    bsp_audio_unlock();
}

static void wake_render_robot_frame(void) {
    if (!s_wake_pill || !s_wake_robot_img || !s_wake_robot_buf) return;
    if (!s_wake_visible_target || lv_obj_has_flag(s_wake_pill, LV_OBJ_FLAG_HIDDEN)) return;
    if (!display_manager_is_on()) return;

    tars_ensure_shared_prefs();
    bool turbo = (s_tars_instance && s_tars_instance->is_turbo_mode());
    const TarsMonoPalette& pal = turbo ? kTarsTurboPal : kTarsPalettes[s_shared_ui_tone];

    const uint16_t c_black = 0x0000;
    const uint16_t c_white = 0xFFFF;
    const uint16_t c_fg    = tars_hex565(pal.fg);
    const uint16_t c_mid   = tars_hex565(pal.mid);
    const uint16_t c_dim   = tars_hex565(pal.dim);
    const uint16_t c_acc   = tars_hex565(pal.acc);

    memset(s_wake_robot_buf, 0, WAKE_MONO_W * WAKE_MONO_H * sizeof(uint16_t));

    int64_t now_us = esp_timer_get_time();
    float t = (float)(now_us / 1000LL) * 0.001f;
    int vis = s_wake_vis_state;
    float energy = (float)s_wake_mic_pct / 100.0f;
    if (vis == TARS_VIS_LISTEN && energy < 0.20f) energy = 0.20f;

    const int widths[4] = { 7, 8, 8, 7 }; // 33 cols wide -> centered inside 35 cols (startX = 1)
    const int sxList[4] = { 1, 9, 18, 27 };
    const int baseY = 1;
    const int slabH = 20;

    for (int i = 0; i < 4; i++) {
        int yShift = 0;
        if (vis == TARS_VIS_LISTEN) {
            int amp = (int)lroundf(energy * 1.8f);
            if (amp > 2) amp = 2;
            yShift = (i == 0 || i == 3) ? -amp : (int)lroundf(sinf(t * 12.0f + (float)i) * 0.6f);
        } else if (vis == TARS_VIS_THINK) {
            yShift = (int)lroundf(sinf(t * 6.0f + (float)i * 1.4f) * 1.6f);
        } else if (turbo) {
            yShift = (((int)(t * 14.0f) + i) % 2 == 0) ? -1 : 1;
        }
        int sx = sxList[i];
        int sw = widths[i];
        int sy = baseY + yShift;
        if (sy < 0) sy = 0;
        if (sy + slabH > WAKE_MONO_GH) sy = WAKE_MONO_GH - slabH;

        mono_px_s(s_wake_robot_buf, WAKE_MONO_W, WAKE_MONO_H, WAKE_MONO_P, sx, sy, sw, slabH, c_fg);
        mono_px_s(s_wake_robot_buf, WAKE_MONO_W, WAKE_MONO_H, WAKE_MONO_P, sx + sw - 1, sy, 1, slabH, c_mid);
        mono_px_s(s_wake_robot_buf, WAKE_MONO_W, WAKE_MONO_H, WAKE_MONO_P, sx + 1, sy, sw - 3, 1, c_white);
        mono_px_s(s_wake_robot_buf, WAKE_MONO_W, WAKE_MONO_H, WAKE_MONO_P, sx, sy + 14, sw, 1, c_black);

        uint16_t dotCol = turbo ? c_acc : c_black;
        if (i == 0 || i == 3) {
            mono_px_s(s_wake_robot_buf, WAKE_MONO_W, WAKE_MONO_H, WAKE_MONO_P, sx + 2, sy + 4, 1, 1, dotCol);
            mono_px_s(s_wake_robot_buf, WAKE_MONO_W, WAKE_MONO_H, WAKE_MONO_P, sx + 4, sy + 4, 1, 1, dotCol);
            mono_px_s(s_wake_robot_buf, WAKE_MONO_W, WAKE_MONO_H, WAKE_MONO_P, sx + 2, sy + 7, 1, 1, dotCol);
            mono_px_s(s_wake_robot_buf, WAKE_MONO_W, WAKE_MONO_H, WAKE_MONO_P, sx + 4, sy + 10, 1, 1, dotCol);
        }
        mono_px_s(s_wake_robot_buf, WAKE_MONO_W, WAKE_MONO_H, WAKE_MONO_P, sx + 2, sy + 16, 1, 3, c_mid);
        mono_px_s(s_wake_robot_buf, WAKE_MONO_W, WAKE_MONO_H, WAKE_MONO_P, sx + sw - 3, sy + 16, 1, 3, c_mid);
    }

    if (turbo) {
        for (int g = 0; g < 3; g++) {
            mono_px_s(s_wake_robot_buf, WAKE_MONO_W, WAKE_MONO_H, WAKE_MONO_P, sxList[g] + widths[g], baseY + 2, 1, slabH - 4, c_acc);
        }
    }

    // Recessed Chest LCD (17x10 across slabs 1 & 2)
    const int lx = sxList[1];
    const int ly = 4;
    const int lw = 17;
    const int lh = 10;
    mono_px_s(s_wake_robot_buf, WAKE_MONO_W, WAKE_MONO_H, WAKE_MONO_P, lx, ly, lw, lh, c_black);
    mono_px_s(s_wake_robot_buf, WAKE_MONO_W, WAKE_MONO_H, WAKE_MONO_P, lx, ly, lw, 1, c_mid);

    if (vis == TARS_VIS_LISTEN) {
        for (int b = 0; b < 7; b++) {
            float wave = 0.45f + 0.55f * fabsf(sinf(t * 13.0f + (float)b * 0.9f));
            int bh = (int)lroundf(energy * 7.8f * wave);
            if (bh < 1) bh = 1;
            if (bh > 7) bh = 7;
            mono_px_s(s_wake_robot_buf, WAKE_MONO_W, WAKE_MONO_H, WAKE_MONO_P, lx + 2 + b * 2, ly + lh - 1 - bh, 1, bh, (bh >= 5) ? c_acc : c_fg);
        }
    } else if (vis == TARS_VIS_THINK) {
        int step = ((int)(t * 8.0f)) % 4;
        int cx = lx + lw / 2;
        int cy = ly + lh / 2;
        mono_px_s(s_wake_robot_buf, WAKE_MONO_W, WAKE_MONO_H, WAKE_MONO_P, cx, cy, 1, 1, c_acc);
        if (step == 0) {
            mono_px_s(s_wake_robot_buf, WAKE_MONO_W, WAKE_MONO_H, WAKE_MONO_P, cx, cy - 2, 1, 2, c_fg);
            mono_px_s(s_wake_robot_buf, WAKE_MONO_W, WAKE_MONO_H, WAKE_MONO_P, cx, cy + 1, 1, 2, c_fg);
        } else if (step == 1) {
            mono_px_s(s_wake_robot_buf, WAKE_MONO_W, WAKE_MONO_H, WAKE_MONO_P, cx + 1, cy - 1, 1, 1, c_fg);
            mono_px_s(s_wake_robot_buf, WAKE_MONO_W, WAKE_MONO_H, WAKE_MONO_P, cx + 2, cy - 2, 1, 1, c_fg);
            mono_px_s(s_wake_robot_buf, WAKE_MONO_W, WAKE_MONO_H, WAKE_MONO_P, cx - 1, cy + 1, 1, 1, c_fg);
            mono_px_s(s_wake_robot_buf, WAKE_MONO_W, WAKE_MONO_H, WAKE_MONO_P, cx - 2, cy + 2, 1, 1, c_fg);
        } else if (step == 2) {
            mono_px_s(s_wake_robot_buf, WAKE_MONO_W, WAKE_MONO_H, WAKE_MONO_P, cx - 2, cy, 2, 1, c_fg);
            mono_px_s(s_wake_robot_buf, WAKE_MONO_W, WAKE_MONO_H, WAKE_MONO_P, cx + 1, cy, 2, 1, c_fg);
        } else {
            mono_px_s(s_wake_robot_buf, WAKE_MONO_W, WAKE_MONO_H, WAKE_MONO_P, cx - 1, cy - 1, 1, 1, c_fg);
            mono_px_s(s_wake_robot_buf, WAKE_MONO_W, WAKE_MONO_H, WAKE_MONO_P, cx - 2, cy - 2, 1, 1, c_fg);
            mono_px_s(s_wake_robot_buf, WAKE_MONO_W, WAKE_MONO_H, WAKE_MONO_P, cx + 1, cy + 1, 1, 1, c_fg);
            mono_px_s(s_wake_robot_buf, WAKE_MONO_W, WAKE_MONO_H, WAKE_MONO_P, cx + 2, cy + 2, 1, 1, c_fg);
        }
    } else if (turbo) {
        int mid = lx + lw / 2;
        mono_px_s(s_wake_robot_buf, WAKE_MONO_W, WAKE_MONO_H, WAKE_MONO_P, mid - 6, ly + 2, 2, 1, c_acc);
        mono_px_s(s_wake_robot_buf, WAKE_MONO_W, WAKE_MONO_H, WAKE_MONO_P, mid - 5, ly + 3, 3, 2, c_fg);
        mono_px_s(s_wake_robot_buf, WAKE_MONO_W, WAKE_MONO_H, WAKE_MONO_P, mid + 4, ly + 2, 2, 1, c_acc);
        mono_px_s(s_wake_robot_buf, WAKE_MONO_W, WAKE_MONO_H, WAKE_MONO_P, mid + 2, ly + 3, 3, 2, c_fg);
        int span = lw - 6;
        int raw = ((int)(t * 16.0f)) % (span * 2);
        int phase = (raw < span) ? raw : (span * 2 - raw);
        mono_px_s(s_wake_robot_buf, WAKE_MONO_W, WAKE_MONO_H, WAKE_MONO_P, lx + 2 + phase, ly + lh - 2, 3, 1, c_acc);
    } else {
        mono_draw_tars_word_s(s_wake_robot_buf, WAKE_MONO_W, WAKE_MONO_H, WAKE_MONO_P, lx + 1, ly + 1, c_fg);
        if (vis == TARS_VIS_REPLY) {
            for (int b = 0; b < 7; b++) {
                int bh = (sinf(t * 14.0f + (float)b * 1.6f) > 0.1f) ? 2 : 1;
                mono_px_s(s_wake_robot_buf, WAKE_MONO_W, WAKE_MONO_H, WAKE_MONO_P, lx + 2 + b * 2, ly + lh - bh, 1, bh, c_acc);
            }
        } else {
            mono_px_s(s_wake_robot_buf, WAKE_MONO_W, WAKE_MONO_H, WAKE_MONO_P, lx + 1, ly + lh - 2, lw - 2, 1, c_dim);
            int sweep = ((int)(t * 4.0f)) % (lw - 4);
            mono_px_s(s_wake_robot_buf, WAKE_MONO_W, WAKE_MONO_H, WAKE_MONO_P, lx + 1 + sweep, ly + lh - 2, 3, 1, c_acc);
        }
    }

    lv_obj_invalidate(s_wake_robot_img);

    // Update live 4.5s countdown progress bar when waiting for voice
    if (s_wake_prog_bar) {
        if (s_wake_countdown_active) {
            int64_t elapsed_ms = (now_us - s_wake_listen_start_us) / 1000LL;
            int rem_pct = 100 - (int)((elapsed_ms * 100LL) / 4500LL);
            if (rem_pct < 0) rem_pct = 0;
            if (rem_pct > 100) rem_pct = 100;
            lv_obj_clear_flag(s_wake_prog_bar, LV_OBJ_FLAG_HIDDEN);
            lv_bar_set_value(s_wake_prog_bar, rem_pct, LV_ANIM_OFF);
        } else {
            lv_obj_add_flag(s_wake_prog_bar, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void on_wake_anim_timer(lv_timer_t* timer) {
    (void)timer;
    wake_render_robot_frame();
}

static void on_wake_exit_completed(lv_anim_t* a) {
    (void)a;
    if (!s_wake_visible_target && s_wake_pill) {
        lv_obj_add_flag(s_wake_pill, LV_OBJ_FLAG_HIDDEN);
    }
}

static void wake_start_diagonal_exit_locked(bool immediate) {
    s_wake_visible_target = false;
    s_wake_countdown_active = false;
    if (s_wake_anim_timer) {
        lv_timer_delete(s_wake_anim_timer);
        s_wake_anim_timer = nullptr;
    }
    if (!s_wake_pill) return;

    lv_anim_delete(s_wake_pill, nullptr);
    if (immediate || lv_obj_has_flag(s_wake_pill, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(s_wake_pill, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    int32_t cur_x = lv_obj_get_x(s_wake_pill);
    int32_t cur_y = lv_obj_get_y(s_wake_pill);

    // Smooth Up-Left Diagonal Retraction (slides cleanly back out of top-left corner!)
    lv_anim_t ax;
    lv_anim_init(&ax);
    lv_anim_set_var(&ax, s_wake_pill);
    lv_anim_set_exec_cb(&ax, (lv_anim_exec_xcb_t)lv_obj_set_x);
    lv_anim_set_values(&ax, cur_x, -280);
    lv_anim_set_duration(&ax, 300);
    lv_anim_set_path_cb(&ax, lv_anim_path_ease_in);
    lv_anim_start(&ax);

    lv_anim_t ay;
    lv_anim_init(&ay);
    lv_anim_set_var(&ay, s_wake_pill);
    lv_anim_set_exec_cb(&ay, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_values(&ay, cur_y, -170);
    lv_anim_set_duration(&ay, 300);
    lv_anim_set_path_cb(&ay, lv_anim_path_ease_in);
    lv_anim_set_completed_cb(&ay, on_wake_exit_completed);
    lv_anim_start(&ay);
}

static void on_wake_pill_clicked(lv_event_t* e) {
    (void)e;
    s_wake_pill_dismissed = true;
    wake_start_diagonal_exit_locked(false);
}

// Show / update Sol Ust Capraz (Top-Left Diagonal) Animated 1-Bit Pixel TARS Wake Overlay
static void update_wake_pill_ui(const char* header_text, const char* body_text, uint32_t accent_hex,
                                bool visible, bool immediate = false) {
    if (!bsp_display_lock(150)) return;
    if (!visible) {
        wake_start_diagonal_exit_locked(immediate);
        bsp_display_unlock();
        return;
    }

    tars_ensure_shared_prefs();
    bool turbo = (s_tars_instance && s_tars_instance->is_turbo_mode());
    const TarsMonoPalette& pal = turbo ? kTarsTurboPal : kTarsPalettes[s_shared_ui_tone];

    if (!s_wake_robot_buf) {
        s_wake_robot_buf = (uint16_t*)heap_caps_calloc(WAKE_MONO_W * WAKE_MONO_H, sizeof(uint16_t),
                                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_wake_robot_buf) {
            s_wake_robot_dsc.data = (const uint8_t*)s_wake_robot_buf;
        }
    }

    if (!s_wake_pill) {
        s_wake_pill = lv_obj_create(lv_layer_top());
        lv_obj_set_width(s_wake_pill, 374);
        lv_obj_set_height(s_wake_pill, LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(s_wake_pill, 96, 0);
        lv_obj_set_style_max_height(s_wake_pill, 240, 0);
        lv_obj_set_pos(s_wake_pill, -260, -150);
        lv_obj_set_style_bg_opa(s_wake_pill, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_wake_pill, 0, 0);
        lv_obj_set_style_pad_all(s_wake_pill, 0, 0);
        lv_obj_set_style_shadow_width(s_wake_pill, 0, 0);
        lv_obj_clear_flag(s_wake_pill, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s_wake_pill, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_wake_pill, on_wake_pill_clicked, LV_EVENT_CLICKED, nullptr);
        lv_obj_add_flag(s_wake_pill, LV_OBJ_FLAG_HIDDEN);

        // Left: 140x88 Live 1-Bit Pixel TARS Monolith Sprite
        if (s_wake_robot_buf) {
            s_wake_robot_img = lv_image_create(s_wake_pill);
            lv_image_set_src(s_wake_robot_img, &s_wake_robot_dsc);
            lv_obj_set_size(s_wake_robot_img, WAKE_MONO_W + 4, WAKE_MONO_H + 4);
            lv_obj_set_pos(s_wake_robot_img, 0, 2);
            lv_obj_set_style_bg_color(s_wake_robot_img, lv_color_hex(0x000000), 0);
            lv_obj_set_style_bg_opa(s_wake_robot_img, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(s_wake_robot_img, 2, 0);
            lv_obj_set_style_radius(s_wake_robot_img, 6, 0);
            lv_obj_set_style_pad_all(s_wake_robot_img, 0, 0);
            lv_obj_add_flag(s_wake_robot_img, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(s_wake_robot_img, on_wake_pill_clicked, LV_EVENT_CLICKED, nullptr);
        }

        // Right: 1-Bit Dark Terminal Speech Card + 4.5s Countdown Rail
        s_wake_card = lv_obj_create(s_wake_pill);
        lv_obj_set_pos(s_wake_card, 150, 2);
        lv_obj_set_width(s_wake_card, 224);
        lv_obj_set_height(s_wake_card, LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(s_wake_card, 92, 0);
        lv_obj_set_style_max_height(s_wake_card, 230, 0);
        lv_obj_set_style_radius(s_wake_card, 8, 0);
        lv_obj_set_style_bg_color(s_wake_card, lv_color_hex(0x05070B), 0);
        lv_obj_set_style_bg_opa(s_wake_card, LV_OPA_90, 0);
        lv_obj_set_style_border_width(s_wake_card, 2, 0);
        lv_obj_set_style_shadow_width(s_wake_card, 0, 0);
        lv_obj_set_style_pad_hor(s_wake_card, 10, 0);
        lv_obj_set_style_pad_ver(s_wake_card, 8, 0);
        lv_obj_set_style_pad_row(s_wake_card, 5, 0);
        lv_obj_set_flex_flow(s_wake_card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(s_wake_card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_scrollbar_mode(s_wake_card, LV_SCROLLBAR_MODE_AUTO);
        lv_obj_add_flag(s_wake_card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_wake_card, on_wake_pill_clicked, LV_EVENT_CLICKED, nullptr);

        s_wake_pill_hdr = lv_label_create(s_wake_card);
        lv_obj_set_width(s_wake_pill_hdr, LV_PCT(100));
        lv_obj_set_style_text_font(s_wake_pill_hdr, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_align(s_wake_pill_hdr, LV_TEXT_ALIGN_LEFT, 0);

        s_wake_pill_lbl = lv_label_create(s_wake_card);
        lv_obj_set_width(s_wake_pill_lbl, LV_PCT(100));
        lv_label_set_long_mode(s_wake_pill_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(s_wake_pill_lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(s_wake_pill_lbl, lv_color_hex(0xF8FAFC), 0);
        lv_obj_set_style_text_align(s_wake_pill_lbl, LV_TEXT_ALIGN_LEFT, 0);

        s_wake_prog_bar = lv_bar_create(s_wake_card);
        lv_obj_set_size(s_wake_prog_bar, LV_PCT(100), 4);
        lv_bar_set_range(s_wake_prog_bar, 0, 100);
        lv_bar_set_value(s_wake_prog_bar, 100, LV_ANIM_OFF);
        lv_obj_set_style_radius(s_wake_prog_bar, 2, 0);
        lv_obj_set_style_radius(s_wake_prog_bar, 2, LV_PART_INDICATOR);
    }

    // Apply current 1-bit palette colors (auto-syncs with saved tone & Turbo Red!)
    uint32_t hdr_col = (accent_hex == 0x00E676 || accent_hex == 0xFFA000) ? pal.acc : accent_hex;
    if (s_wake_robot_img) {
        lv_obj_set_style_border_color(s_wake_robot_img, lv_color_hex(pal.mid), 0);
    }
    if (s_wake_card) {
        lv_obj_set_style_border_color(s_wake_card, lv_color_hex(pal.mid), 0);
    }
    if (s_wake_pill_hdr) {
        lv_obj_set_style_text_color(s_wake_pill_hdr, lv_color_hex(hdr_col), 0);
        lv_label_set_text(s_wake_pill_hdr, header_text ? header_text : "TARS");
    }
    if (s_wake_pill_lbl && body_text) {
        lv_label_set_text(s_wake_pill_lbl, body_text);
    }
    if (s_wake_prog_bar) {
        lv_obj_set_style_bg_color(s_wake_prog_bar, lv_color_hex(pal.dim), LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_wake_prog_bar, lv_color_hex(pal.acc), LV_PART_INDICATOR);
    }
    if (s_wake_card) {
        lv_obj_scroll_to_y(s_wake_card, 0, LV_ANIM_OFF);
    }

    bool needs_enter_anim = (!s_wake_visible_target || lv_obj_has_flag(s_wake_pill, LV_OBJ_FLAG_HIDDEN));
    s_wake_visible_target = true;

    if (needs_enter_anim) {
        lv_anim_delete(s_wake_pill, nullptr);
        lv_obj_set_pos(s_wake_pill, -260, -150);
        lv_obj_clear_flag(s_wake_pill, LV_OBJ_FLAG_HIDDEN);

        // Sol Ust Capraz Entrance Animation (-260,-150) -> (18,52) with spring overshoot!
        lv_anim_t ax;
        lv_anim_init(&ax);
        lv_anim_set_var(&ax, s_wake_pill);
        lv_anim_set_exec_cb(&ax, (lv_anim_exec_xcb_t)lv_obj_set_x);
        lv_anim_set_values(&ax, -260, 18);
        lv_anim_set_duration(&ax, 380);
        lv_anim_set_path_cb(&ax, lv_anim_path_overshoot);
        lv_anim_start(&ax);

        lv_anim_t ay;
        lv_anim_init(&ay);
        lv_anim_set_var(&ay, s_wake_pill);
        lv_anim_set_exec_cb(&ay, (lv_anim_exec_xcb_t)lv_obj_set_y);
        lv_anim_set_values(&ay, -150, 52);
        lv_anim_set_duration(&ay, 380);
        lv_anim_set_path_cb(&ay, lv_anim_path_overshoot);
        lv_anim_start(&ay);
    }

    wake_render_robot_frame();
    if (!s_wake_anim_timer) {
        s_wake_anim_timer = lv_timer_create(on_wake_anim_timer, 90, nullptr); // ~11 FPS crisp wake robot EQ
    }
    bsp_display_unlock();
}

// Extract ALL [ACTION: ...] blocks from anywhere in raw_reply (start, middle, OR end!)
// while preserving 100% of surrounding Turkish spoken text in out_clean_utf8.
static void tars_extract_and_strip_actions(const char* raw_reply,
                                           char* out_action_inner, size_t max_act,
                                           char* out_clean_utf8, size_t max_clean) {
    if (out_action_inner && max_act > 0) out_action_inner[0] = '\0';
    if (out_clean_utf8 && max_clean > 0) out_clean_utf8[0] = '\0';
    if (!raw_reply) return;

    size_t act_len = 0;
    size_t cln_len = 0;
    const char* cur = raw_reply;

    while (*cur) {
        const char* tag = strstr(cur, "[ACTION:");
        if (!tag) {
            size_t rem = strlen(cur);
            if (out_clean_utf8 && max_clean > 1 && cln_len < max_clean - 1) {
                size_t copy_n = (rem < (max_clean - 1 - cln_len)) ? rem : (max_clean - 1 - cln_len);
                memcpy(out_clean_utf8 + cln_len, cur, copy_n);
                cln_len += copy_n;
                out_clean_utf8[cln_len] = '\0';
            }
            break;
        }

        // Copy spoken text BEFORE this [ACTION: ...] tag
        if (tag > cur && out_clean_utf8 && max_clean > 1 && cln_len < max_clean - 1) {
            size_t pre = (size_t)(tag - cur);
            size_t copy_n = (pre < (max_clean - 1 - cln_len)) ? pre : (max_clean - 1 - cln_len);
            memcpy(out_clean_utf8 + cln_len, cur, copy_n);
            cln_len += copy_n;
            out_clean_utf8[cln_len] = '\0';
        }

        const char* close_br = strchr(tag, ']');
        if (!close_br) {
            // Incomplete/unclosed tag at tail -> drop it cleanly so raw syntax never leaks
            break;
        }

        const char* inner = tag + 8;
        while (inner < close_br && (*inner == ' ' || *inner == '\t')) inner++;
        const char* inner_end = close_br;
        while (inner_end > inner && (inner_end[-1] == ' ' || inner_end[-1] == '\t')) inner_end--;
        size_t ilen = (size_t)(inner_end - inner);
        if (ilen > 0 && ilen < 240 && out_action_inner && max_act > 1) {
            if (act_len > 0 && act_len + 2 < max_act - 1) {
                out_action_inner[act_len++] = ';';
                out_action_inner[act_len++] = ' ';
            }
            size_t copy_a = (ilen < (max_act - 1 - act_len)) ? ilen : (max_act - 1 - act_len);
            memcpy(out_action_inner + act_len, inner, copy_a);
            act_len += copy_a;
            out_action_inner[act_len] = '\0';
        }

        cur = close_br + 1;
        while (*cur == ' ' || *cur == '\n' || *cur == '\r') cur++;
    }

    // Trim leading/trailing spaces & newlines from out_clean_utf8
    if (out_clean_utf8 && cln_len > 0) {
        size_t lead = 0;
        while (out_clean_utf8[lead] == ' ' || out_clean_utf8[lead] == '\n' || out_clean_utf8[lead] == '\r' || out_clean_utf8[lead] == '\t') {
            lead++;
        }
        if (lead > 0) {
            cln_len -= lead;
            memmove(out_clean_utf8, out_clean_utf8 + lead, cln_len + 1);
        }
        while (cln_len > 0 && (out_clean_utf8[cln_len - 1] == ' ' || out_clean_utf8[cln_len - 1] == '\n' ||
                               out_clean_utf8[cln_len - 1] == '\r' || out_clean_utf8[cln_len - 1] == '\t')) {
            out_clean_utf8[--cln_len] = '\0';
        }
    }
}

// Finalize & execute structured actions honestly AFTER Groq JSON returns (zero nested TLS, truthful error feedback!)
static void tars_finalize_reply(const char* raw_reply, const char* structured_actions,
                                char* out_clean_reply, size_t max_reply,
                                char* out_action_badge, size_t max_badge,
                                const char* user_transcript_ascii = nullptr) {
    out_clean_reply[0] = '\0';
    strncpy(out_action_badge, "EYLEM: YOK (SOHBET)", max_badge - 1);
    out_action_badge[max_badge - 1] = '\0';
    if (!raw_reply) return;

    char act_cmd[256] = {0};
    char* clean_utf8 = (char*)heap_caps_calloc(1, TARS_TEXT_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!clean_utf8) clean_utf8 = (char*)calloc(1, TARS_TEXT_CAP);
    if (!clean_utf8) return;

    if (structured_actions && structured_actions[0] != '\0') {
        strncpy(act_cmd, structured_actions, sizeof(act_cmd) - 1);
        strncpy(clean_utf8, raw_reply, TARS_TEXT_CAP - 1);
    } else {
        tars_extract_and_strip_actions(raw_reply, act_cmd, sizeof(act_cmd), clean_utf8, TARS_TEXT_CAP);
    }

    bool pc_unreachable = false;
    bool tv_unreachable = false;
    bool action_triggered = false;
    if (act_cmd[0] != '\0') {
        action_triggered = tars_fire_action_tag(act_cmd, user_transcript_ascii, out_action_badge, max_badge,
                                                &pc_unreachable, &tv_unreachable);
    }

    // Honest reply overrides: NEVER gaslight user if PC/TV failed or if real PC stats were fetched!
    if (pc_unreachable) {
        snprintf(clean_utf8, TARS_TEXT_CAP - 1,
                 "Bilgisayara ulasilamadi (%s:5000). UZ_PC_Agent ve Wi-Fi agini kontrol et Eymen.",
                 pc_control_get_cached_ip());
    } else if (tv_unreachable) {
        snprintf(clean_utf8, TARS_TEXT_CAP - 1,
                 "Televizyona ulasilamadi. TV'nin acik ve ayni Wi-Fi aginda oldugunu kontrol et Eymen.");
    } else if (s_live_pc_telemetry[0] != '\0') {
        // Real live CPU/RAM/Disk telemetry replaces speculative LLM numbers!
        snprintf(clean_utf8, TARS_TEXT_CAP - 1, "PC Canli Durum: %s", s_live_pc_telemetry);
    }

    // Send full UTF-8 spoken response to phone over BLE (serialized on ble_evt_wrk with MTU clamp)
    if (ble_manager_is_connected() && strlen(clean_utf8) > 0) {
        char ble_cmd[240];
        snprintf(ble_cmd, sizeof(ble_cmd), "TARS_SPEAK|%.224s", clean_utf8);
        ble_manager_send_media_command(ble_cmd);
    }

    tars_utf8_to_ascii(out_clean_reply, clean_utf8, max_reply);
    free(clean_utf8);

    if (strlen(out_clean_reply) == 0 && action_triggered) {
        strncpy(out_clean_reply, "Komut uygulandi Eymen.", max_reply - 1);
    }

    // Stagger motor haptic and speaker PA strictly so 5V audio boost + vibration NEVER overlap (zero brownout!)
    bsp_vibrate_pulse(action_triggered ? 65 : 35);
    vTaskDelay(pdMS_TO_TICKS(130));
    tars_play_acoustic_response(action_triggered);
}

// Local Turkish Fast-Intent Brain (ONLY runs if offline/cloud fails; Turkish UTF-8 normalized + PC-priority safe!)
static void tars_local_turkish_fallback(const char* transcript, const char* fail_reason,
                                        char* out_raw_reply, size_t max_len) {
    char ascii_tr[256] = {0};
    tars_utf8_to_ascii(ascii_tr, transcript ? transcript : "", sizeof(ascii_tr));
    char low[256] = {0};
    for (size_t i = 0; ascii_tr[i] && i < sizeof(low) - 1; i++) {
        low[i] = (char)tolower((unsigned char)ascii_tr[i]);
    }

    bool has_pc = (strstr(low, "bilgisayar") || strstr(low, "pc"));
    bool has_light = (strstr(low, "salon") || strstr(low, "isik") || strstr(low, "lamba"));

    if (has_pc && strstr(low, "uyut")) {
        strncpy(out_raw_reply, "[ACTION: PC_SLEEP] Bilgisayari uykuya aldim.", max_len - 1);
    } else if (has_pc && strstr(low, "kilit")) {
        strncpy(out_raw_reply, "[ACTION: PC_LOCK] Bilgisayari kilitledim.", max_len - 1);
    } else if (has_pc && strstr(low, "kapat")) {
        strncpy(out_raw_reply, "[ACTION: PC_SHUTDOWN_SEC:30] Bilgisayari 30 saniye icinde kapatiyorum.", max_len - 1);
    } else if (has_pc && (strstr(low, "ac") || strstr(low, "uyandir"))) {
        strncpy(out_raw_reply, "[ACTION: PC_WAKE] Wake-on-LAN paketini gonderdim.", max_len - 1);
    } else if (has_light && (strstr(low, "kapat") || strstr(low, "sondur"))) {
        strncpy(out_raw_reply, "[ACTION: SALON_OFF] Salon isigini kapattim Eymen.", max_len - 1);
    } else if (has_light && (strstr(low, "ac") || strstr(low, "yak"))) {
        strncpy(out_raw_reply, "[ACTION: SALON_ON] Salon isigini actim Eymen.", max_len - 1);
    } else if (strstr(low, "saat kac")) {
        struct tm ti = {};
        rtc_get_time(&ti);
        snprintf(out_raw_reply, max_len - 1, "Saat su an %02d:%02d Eymen.", ti.tm_hour, ti.tm_min);
    } else if (strstr(low, "pil") || strstr(low, "sarj")) {
        int pct = bsp_battery_get_percent();
        snprintf(out_raw_reply, max_len - 1, "Pil seviyem su an yuzde %d.", pct);
    } else {
        snprintf(out_raw_reply, max_len - 1, "%s",
                 fail_reason ? fail_reason : "Bulut baglantisi kurulamadi. Ev Wi-Fi agini kontrol et.");
    }
    out_raw_reply[max_len - 1] = '\0';
}

// Standard ITU-T G.711 u-Law encoder (compresses 16-bit PCM -> 8-bit u-Law = 2x faster Wi-Fi upload!)
static inline uint8_t linear16_to_ulaw(int16_t sample) {
    const int BIAS = 0x84;
    const int CLIP = 32635;
    if (sample == -32768) sample = -32767;
    int sign = (sample < 0) ? 0x80 : 0x00;
    int mag = (sample < 0) ? -sample : sample;
    if (mag > CLIP) mag = CLIP;
    mag += BIAS;
    int exponent = 7;
    for (int exp_mask = 0x4000; (mag & exp_mask) == 0 && exponent > 0; exponent--, exp_mask >>= 1) {}
    int mantissa = (mag >> (exponent + 3)) & 0x0F;
    return (uint8_t)(~(sign | (exponent << 4) | mantissa));
}

// Preprocess 16-bit PCM (DC removal + soft-speech peak AGC) and encode straight into 2x smaller 16kHz u-Law WAV!
static size_t tars_encode_ulaw_wav(int16_t* pcm, size_t num_samples, uint8_t* out_wav) {
    if (!pcm || !out_wav || num_samples == 0) return 0;

    int64_t sum = 0;
    for (size_t i = 0; i < num_samples; i++) sum += pcm[i];
    int32_t dc_offset = (int32_t)(sum / (int64_t)num_samples);

    int32_t peak = 1;
    for (size_t i = 0; i < num_samples; i++) {
        int32_t v = (int32_t)pcm[i] - dc_offset;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        pcm[i] = (int16_t)v;
        int32_t av = (v < 0) ? -v : v;
        if (av > peak) peak = av;
    }

    float gain = 1.0f;
    if (peak > 450 && peak < 20000) {
        gain = 24000.0f / (float)peak;
        if (gain > 6.0f) gain = 6.0f;
        if (gain < 1.15f) gain = 1.0f;
    }

    // Standard 58-byte WAV header for WAVE_FORMAT_MULAW (7)
    uint32_t data_size = (uint32_t)num_samples;
    uint32_t file_size = 50 + data_size;
    uint32_t fmt_size = 18;
    uint16_t audio_fmt = 7; // WAVE_FORMAT_MULAW
    uint16_t num_channels = 1;
    uint32_t sample_rate = TARS_SAMPLE_RATE;
    uint32_t byte_rate = TARS_SAMPLE_RATE;
    uint16_t block_align = 1;
    uint16_t bits_per_sample = 8;
    uint16_t cb_size = 0;
    uint32_t fact_size = 4;

    memcpy(out_wav + 0, "RIFF", 4);
    memcpy(out_wav + 4, &file_size, 4);
    memcpy(out_wav + 8, "WAVE", 4);
    memcpy(out_wav + 12, "fmt ", 4);
    memcpy(out_wav + 16, &fmt_size, 4);
    memcpy(out_wav + 20, &audio_fmt, 2);
    memcpy(out_wav + 22, &num_channels, 2);
    memcpy(out_wav + 24, &sample_rate, 4);
    memcpy(out_wav + 28, &byte_rate, 4);
    memcpy(out_wav + 32, &block_align, 2);
    memcpy(out_wav + 34, &bits_per_sample, 2);
    memcpy(out_wav + 36, &cb_size, 2);
    memcpy(out_wav + 38, "fact", 4);
    memcpy(out_wav + 42, &fact_size, 4);
    memcpy(out_wav + 46, &data_size, 4);
    memcpy(out_wav + 50, "data", 4);
    memcpy(out_wav + 54, &data_size, 4);

    uint8_t* dst = out_wav + TARS_ULAW_HDR_BYTES;
    if (gain > 1.0f) {
        for (size_t i = 0; i < num_samples; i++) {
            int32_t v = (int32_t)((float)pcm[i] * gain);
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            dst[i] = linear16_to_ulaw((int16_t)v);
        }
    } else {
        for (size_t i = 0; i < num_samples; i++) {
            dst[i] = linear16_to_ulaw(pcm[i]);
        }
    }
    return TARS_ULAW_HDR_BYTES + data_size;
}

// Persistent HTTP JSON Context for Groq Keep-Alive Client (v4.0 Strict Structured JSON)
struct GroqHttpCtx {
    char* json_buf;
    size_t json_len;
    size_t json_cap;
};

static GroqHttpCtx s_http_ctx = {};

static esp_err_t groq_http_event_handler(esp_http_client_event_t *evt) {
    GroqHttpCtx* ctx = (GroqHttpCtx*)evt->user_data;
    if (!ctx || evt->event_id != HTTP_EVENT_ON_DATA || evt->data_len <= 0 || !ctx->json_buf) {
        return ESP_OK;
    }
    size_t copy_len = (size_t)evt->data_len;
    if (ctx->json_len + copy_len >= ctx->json_cap) {
        copy_len = (ctx->json_cap - 1) - ctx->json_len;
    }
    if (copy_len > 0) {
        memcpy(ctx->json_buf + ctx->json_len, evt->data, copy_len);
        ctx->json_len += copy_len;
        ctx->json_buf[ctx->json_len] = '\0';
    }
    return ESP_OK;
}

static void tars_reset_groq_client(void) {
    if (s_groq_client) {
        esp_http_client_cleanup(s_groq_client);
        s_groq_client = nullptr;
    }
    s_groq_warm = false;
}

static esp_http_client_handle_t tars_get_groq_client(void) {
    int64_t now = esp_timer_get_time();
    if (s_groq_client && s_groq_warm && (now - s_groq_last_use_us) > 45000000LL) {
        tars_reset_groq_client();
    }
    if (!s_groq_client) {
        esp_http_client_config_t config = {};
        config.url = "https://api.groq.com/openai/v1/chat/completions";
        config.event_handler = groq_http_event_handler;
        config.user_data = &s_http_ctx;
        config.crt_bundle_attach = esp_crt_bundle_attach;
        config.timeout_ms = 14000;
        config.keep_alive_enable = true;
        config.keep_alive_idle = 40;
        config.keep_alive_interval = 10;
        config.keep_alive_count = 3;
        config.buffer_size = 2048;
        config.buffer_size_tx = 4096;
        s_groq_client = esp_http_client_init(&config);
        s_groq_warm = false;
    }
    return s_groq_client;
}

enum TarsPipelineResult {
    TARS_RES_OK = 0,
    TARS_RES_NO_WIFI = 1,
    TARS_RES_SILENCE_HALLUCINATION = 2,
    TARS_RES_CLOUD_ERR = 3,
};

struct TarsTimingStats {
    int stt_ms;
    int llm_ms;
    int total_ms;
    bool was_warm_tls;
};

// Build Groq Strict JSON Schema payload: guarantees {"reply": string, "actions": string[]} with 0% tag hallucination!
static char* tars_build_groq_json_body(const char* sys_prompt, const char* raw_transcript) {
    cJSON* root = cJSON_CreateObject();
    if (!root) return nullptr;

    cJSON_AddStringToObject(root, "model", "openai/gpt-oss-20b");
    cJSON_AddStringToObject(root, "reasoning_effort", "low");
    cJSON_AddBoolToObject(root, "include_reasoning", false);
    cJSON_AddNumberToObject(root, "max_tokens", 512);
    cJSON_AddNumberToObject(root, "temperature", 0.15);

    // Strict Structured Output schema: {"reply": string, "actions": string[]}
    cJSON* rf = cJSON_AddObjectToObject(root, "response_format");
    cJSON_AddStringToObject(rf, "type", "json_schema");
    cJSON* js = cJSON_AddObjectToObject(rf, "json_schema");
    cJSON_AddStringToObject(js, "name", "tars_response");
    cJSON_AddBoolToObject(js, "strict", true);
    cJSON* sch = cJSON_AddObjectToObject(js, "schema");
    cJSON_AddStringToObject(sch, "type", "object");
    cJSON* props = cJSON_AddObjectToObject(sch, "properties");
    cJSON* p_rep = cJSON_AddObjectToObject(props, "reply");
    cJSON_AddStringToObject(p_rep, "type", "string");
    cJSON* p_act = cJSON_AddObjectToObject(props, "actions");
    cJSON_AddStringToObject(p_act, "type", "array");
    cJSON* items = cJSON_AddObjectToObject(p_act, "items");
    cJSON_AddStringToObject(items, "type", "string");
    cJSON* req_arr = cJSON_AddArrayToObject(sch, "required");
    cJSON_AddItemToArray(req_arr, cJSON_CreateString("reply"));
    cJSON_AddItemToArray(req_arr, cJSON_CreateString("actions"));
    cJSON_AddBoolToObject(sch, "additionalProperties", false);

    cJSON* msgs = cJSON_AddArrayToObject(root, "messages");
    cJSON* m_sys = cJSON_CreateObject();
    cJSON_AddStringToObject(m_sys, "role", "system");
    cJSON_AddStringToObject(m_sys, "content", sys_prompt);
    cJSON_AddItemToArray(msgs, m_sys);

    for (int h = 0; h < s_hist_count; h++) {
        if (s_hist_user[h] && s_hist_asst[h] && strlen(s_hist_user[h]) > 0) {
            cJSON* hu = cJSON_CreateObject();
            cJSON_AddStringToObject(hu, "role", "user");
            cJSON_AddStringToObject(hu, "content", s_hist_user[h]);
            cJSON_AddItemToArray(msgs, hu);

            cJSON* ha = cJSON_CreateObject();
            cJSON_AddStringToObject(ha, "role", "assistant");
            cJSON_AddStringToObject(ha, "content", s_hist_asst[h]);
            cJSON_AddItemToArray(msgs, ha);
        }
    }

    cJSON* m_usr = cJSON_CreateObject();
    cJSON_AddStringToObject(m_usr, "role", "user");
    cJSON_AddStringToObject(m_usr, "content", raw_transcript);
    cJSON_AddItemToArray(msgs, m_usr);

    char* body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;
}

// TARS v4.0 Clean Pipeline: u-Law Whisper v3 STT (2-retry) -> Groq Strict JSON Schema (2-retry) -> Deterministic Actions!
static TarsPipelineResult tars_run_groq_turbo_pipeline(const uint8_t* wav_data, size_t wav_len,
                                                       bool is_wake_window,
                                                       char* out_transcript_ascii, size_t max_trans,
                                                       char* out_reply_utf8, size_t max_reply,
                                                       char* out_actions_joined, size_t max_actions,
                                                       TarsTimingStats* out_stats) {
    memset(out_stats, 0, sizeof(*out_stats));
    out_actions_joined[0] = '\0';
    s_live_pc_telemetry[0] = '\0';
    tars_load_api_key();
    if (strlen(s_groq_api_key) < 15) return TARS_RES_CLOUD_ERR;
    if (wifi_manager_is_ap_active()) return TARS_RES_NO_WIFI;

    if (!wifi_manager_is_connected()) {
        if (is_wake_window) {
            s_wake_vis_state = TARS_VIS_THINK;
            s_wake_countdown_active = false;
            update_wake_pill_ui("TARS // WI-FI", "Ev Wi-Fi baglaniyor...", 0xFFA000, true);
        } else if (s_tars_instance) {
            s_tars_instance->set_visual_state(TARS_VIS_THINK, 0);
            s_tars_instance->update_ui_state("WI-FI BAGLANIYOR...", 0xFFA000, 0, nullptr, nullptr, nullptr);
        }
        if (!wifi_manager_is_active()) {
            wifi_manager_start_async();
        } else if (wifi_manager_get_state() == WIFI_STATE_IDLE || wifi_manager_get_state() == WIFI_STATE_FAILED) {
            wifi_manager_connect_default_async();
        }
        for (int w = 0; w < 35 && !wifi_manager_is_connected() && !s_tars_abort_audio; w++) {
            display_manager_reset_timer();
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        if (!wifi_manager_is_connected()) return TARS_RES_NO_WIFI;
    }

    int64_t t_pipe_start = esp_timer_get_time();
    display_manager_reset_timer();

    if (is_wake_window) {
        s_wake_vis_state = TARS_VIS_THINK;
        s_wake_countdown_active = false;
        update_wake_pill_ui("TARS // STT", "Turkce konusma cozuluyor...", 0x00E5FF, true);
    } else if (s_tars_instance) {
        s_tars_instance->set_visual_state(TARS_VIS_THINK, 0);
        s_tars_instance->update_ui_state("SES COZULUYOR...", 0x00E5FF, 0, nullptr, nullptr, nullptr);
    }

    char* resp_mem = (char*)heap_caps_malloc(12288, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!resp_mem) return TARS_RES_CLOUD_ERR;

    char auth_hdr[200];
    snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", s_groq_api_key);

    // Build multipart/form-data body in PSRAM (u-law WAV is 2x smaller = 2x faster upload!)
    const char* boundary = "----TarsWatchBoundary7MA4YWxk";
    const char* part_head =
        "------TarsWatchBoundary7MA4YWxk\r\n"
        "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
        "whisper-large-v3\r\n"
        "------TarsWatchBoundary7MA4YWxk\r\n"
        "Content-Disposition: form-data; name=\"language\"\r\n\r\n"
        "tr\r\n"
        "------TarsWatchBoundary7MA4YWxk\r\n"
        "Content-Disposition: form-data; name=\"temperature\"\r\n\r\n"
        "0\r\n"
        "------TarsWatchBoundary7MA4YWxk\r\n"
        "Content-Disposition: form-data; name=\"prompt\"\r\n\r\n"
        "TARS, Eymen, Tivibu, YouTube, Netflix, Spotify, KiCad, Creality Print, Valorant, Minecraft, OpenRocket, Discord, Steam, Antigravity, WhatsApp.\r\n"
        "------TarsWatchBoundary7MA4YWxk\r\n"
        "Content-Disposition: form-data; name=\"response_format\"\r\n\r\n"
        "json\r\n"
        "------TarsWatchBoundary7MA4YWxk\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"voice.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n";
    const char* part_tail = "\r\n------TarsWatchBoundary7MA4YWxk--\r\n";

    size_t head_len = strlen(part_head);
    size_t tail_len = strlen(part_tail);
    size_t body_len = head_len + wav_len + tail_len;

    uint8_t* body_buf = (uint8_t*)heap_caps_malloc(body_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body_buf) {
        free(resp_mem);
        return TARS_RES_CLOUD_ERR;
    }
    memcpy(body_buf, part_head, head_len);
    memcpy(body_buf + head_len, wav_data, wav_len);
    memcpy(body_buf + head_len + wav_len, part_tail, tail_len);

    char ctype_hdr[96];
    snprintf(ctype_hdr, sizeof(ctype_hdr), "multipart/form-data; boundary=%s", boundary);

    wifi_manager_mark_activity();
    wifi_manager_set_turbo(true);

    out_stats->was_warm_tls = s_groq_warm;
    esp_err_t err = ESP_FAIL;
    int status_code = 0;

    // Step 1: Whisper-Large-v3 STT (2 attempts with automatic stale-socket reset)
    for (int attempt = 0; attempt < 2; attempt++) {
        esp_http_client_handle_t client = tars_get_groq_client();
        if (!client) break;

        s_http_ctx.json_buf = resp_mem;
        s_http_ctx.json_len = 0;
        s_http_ctx.json_cap = 12288;
        resp_mem[0] = '\0';

        esp_http_client_set_url(client, "https://api.groq.com/openai/v1/audio/transcriptions");
        esp_http_client_set_method(client, HTTP_METHOD_POST);
        esp_http_client_set_header(client, "Authorization", auth_hdr);
        esp_http_client_set_header(client, "Content-Type", ctype_hdr);
        esp_http_client_set_post_field(client, (const char*)body_buf, body_len);

        err = esp_http_client_perform(client);
        status_code = esp_http_client_get_status_code(client);
        if (err == ESP_OK && status_code == 200) {
            s_groq_warm = true;
            s_groq_last_use_us = esp_timer_get_time();
            break;
        }
        ESP_LOGW(TAG, "Groq STT attempt %d failed (err=%s, http=%d, body='%.100s') -> resetting TLS",
                 attempt + 1, esp_err_to_name(err), status_code, resp_mem);
        tars_reset_groq_client();
        out_stats->was_warm_tls = false;
        vTaskDelay(pdMS_TO_TICKS(80));
    }

    free(body_buf);
    int64_t t_stt_done = esp_timer_get_time();
    out_stats->stt_ms = (int)((t_stt_done - t_pipe_start) / 1000);
    display_manager_reset_timer();

    if (err != ESP_OK || status_code != 200) {
        wifi_manager_set_turbo(false);
        free(resp_mem);
        return TARS_RES_CLOUD_ERR;
    }

    cJSON* stt_json = cJSON_Parse(resp_mem);
    if (!stt_json) {
        wifi_manager_set_turbo(false);
        free(resp_mem);
        return TARS_RES_CLOUD_ERR;
    }
    cJSON* text_item = cJSON_GetObjectItem(stt_json, "text");
    if (!text_item || !cJSON_IsString(text_item) || strlen(text_item->valuestring) < 2) {
        cJSON_Delete(stt_json);
        wifi_manager_set_turbo(false);
        free(resp_mem);
        return TARS_RES_SILENCE_HALLUCINATION;
    }

    char* raw_transcript = (char*)heap_caps_malloc(TARS_TEXT_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!raw_transcript) {
        cJSON_Delete(stt_json);
        wifi_manager_set_turbo(false);
        free(resp_mem);
        return TARS_RES_CLOUD_ERR;
    }
    const char* t_src = text_item->valuestring;
    while (*t_src == ' ' || *t_src == '\n' || *t_src == '\r') t_src++;
    strncpy(raw_transcript, t_src, TARS_TEXT_CAP - 1);
    raw_transcript[TARS_TEXT_CAP - 1] = '\0';
    tars_utf8_to_ascii(out_transcript_ascii, raw_transcript, max_trans);
    cJSON_Delete(stt_json);

    ESP_LOGI(TAG, "STT (%dms, %uKB u-law): '%s'",
             out_stats->stt_ms, (unsigned)(wav_len / 1024), out_transcript_ascii);
    if (tars_is_whisper_hallucination(out_transcript_ascii)) {
        ESP_LOGW(TAG, "Filtered Whisper silence hallucination: '%s'", out_transcript_ascii);
        free(raw_transcript);
        wifi_manager_set_turbo(false);
        free(resp_mem);
        return TARS_RES_SILENCE_HALLUCINATION;
    }

    if (is_wake_window) {
        s_wake_vis_state = TARS_VIS_THINK;
        s_wake_countdown_active = false;
        update_wake_pill_ui("TARS // DUSUNUYOR", out_transcript_ascii, 0xFFA000, true);
    } else if (s_tars_instance) {
        s_tars_instance->set_visual_state(TARS_VIS_THINK, 0);
        s_tars_instance->update_ui_state("TARS DUSUNUYOR...", 0xFFA000, 0, out_transcript_ascii, nullptr, nullptr);
    }

    // Step 2: Groq Structured JSON Brain (Strict json_schema + 2-attempt TLS retry!)
    int humor = s_tars_instance ? s_tars_instance->get_humor() : 75;
    int bat = bsp_battery_get_percent();
    struct tm ti = {};
    rtc_get_time(&ti);

    char* sys_prompt = (char*)heap_caps_malloc(3584, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!sys_prompt) {
        free(raw_transcript);
        wifi_manager_set_turbo(false);
        free(resp_mem);
        return TARS_RES_CLOUD_ERR;
    }
    snprintf(sys_prompt, 3584,
             "Sen Interstellar filmindeki TARS'sin. Sahibin Eymen'in akilli saatindeki zeki, sadik, esprili sesli asistansin. "
             "Mizah=%%%d, Durustluk=%%90. Bilgi: Saat=%02d:%02d, SaatPili=%%%d.\n"
             "HER ZAMAN sadece su JSON formatinda cevap ver: {\"reply\": \"kisa 1-2 cumle Turkce cevap\", \"actions\": [\"KOMUT\"]}\n"
             "KURALLAR:\n"
             "1) Normal sohbet, selam, soru, fikra, saat/pil sorma, dertlesme -> actions KESINLIKLE bos liste [] olmali! Asla kendiliginden isik/PC/TV acip kapatma.\n"
             "2) SADECE kullanici acikca bir cihazi/uygulamayi/modu kontrol etmek isterse actions listesine asagidaki gecerli komut kodlarini koy:\n"
             "- Isik: SALON_ON, SALON_OFF, SALON_TOGGLE\n"
             "- TV: TV_POWER, TV_WAKE, TV_SLEEP, TV_PLAY_PAUSE, TV_PAUSE, TV_PLAY, TV_VOL_UP, TV_VOL_DOWN, TV_MUTE, TV_CH_UP, TV_CH_DOWN, TV_FWD, TV_REW, TV_NEXT, TV_PREV, TV_HOME, TV_BACK, TV_OK, TV_OPEN_YOUTUBE, TV_OPEN_NETFLIX, TV_OPEN_SPOTIFY, TV_OPEN_PRIME, TV_OPEN_DISNEY, TV_YOUTUBE_PLAY:<sorgu>\n"
             "- PC Guc: PC_WAKE, PC_SLEEP, PC_LOCK, PC_SCREEN_OFF, PC_SHUTDOWN_SEC:<sn>, PC_SHUTDOWN_MIN:<dk>, PC_SHUTDOWN_CANCEL\n"
             "- PC Durum (CPU/RAM/disk/bilgisayar durumu sorulunca MUTLAKA kullan!): PC_GET_STATS\n"
             "- PC Medya/Ses: PC_MEDIA_PLAY_PAUSE, PC_MEDIA_NEXT, PC_MEDIA_PREV, PC_VOL_UP, PC_VOL_DOWN, PC_VOL_MUTE, PC_VOL_SET:<0-100>\n"
             "- PC Web/Arama/Yaz: PC_GOOGLE_SEARCH:<sorgu>, PC_OPEN_URL:<url>, PC_YOUTUBE_PLAY:<sorgu>, PC_YOUTUBE_SEARCH:<sorgu>, PC_SPOTIFY_SEARCH:<sorgu>, PC_TYPE_TEXT:<metin>, PC_OPEN_CLIPBOARD_URL\n"
             "- PC Uygulama: PC_OPEN_APP:<ad> (KiCad, Creality Print, Valorant, LoL, Minecraft, OpenRocket, Betaflight, OBS, CapCut, Telegram, Antigravity, Spotify, Chrome, Discord, Steam, VS Code, Arduino, Ayarlar), PC_CLOSE_APP:<ad>\n"
             "- PC Video/Pencere: PC_SEEK_FWD, PC_SEEK_BACK, PC_CAPTIONS, PC_THEATER, PC_FULLSCREEN, PC_SPEED_UP, PC_SPEED_DOWN, PC_NEW_TAB, PC_REOPEN_TAB, PC_NEXT_TAB, PC_PREV_TAB, PC_CLOSE_TAB, PC_REFRESH, PC_SCROLL_DOWN, PC_SCROLL_UP, PC_ALT_TAB, PC_CLOSE_WINDOW, PC_SNAP_LEFT, PC_SNAP_RIGHT, PC_MINIMIZE_ALL, PC_BOSS_KEY, PC_SCREENSHOT\n"
             "- Mod/Tema: PC_MODE_GAME (oyun modu), PC_MODE_STUDY (ders modu), PC_MODE_MOVIE (sinema modu), PC_MODE_BREAK (mola modu), TARS_TURBO_ON (kirmizi turbo tema), TARS_TURBO_OFF (normal tema), HUMOR_SET:<0-100>",
             humor, ti.tm_hour, ti.tm_min, bat);

    char* chat_body = tars_build_groq_json_body(sys_prompt, raw_transcript);
    free(sys_prompt);

    int64_t t_chat_start = esp_timer_get_time();
    out_reply_utf8[0] = '\0';
    TarsPipelineResult res = TARS_RES_CLOUD_ERR;

    if (chat_body) {
        size_t chat_body_len = strlen(chat_body);
        for (int attempt = 0; attempt < 2; attempt++) {
            esp_http_client_handle_t client = tars_get_groq_client();
            if (!client) break;

            s_http_ctx.json_buf = resp_mem;
            s_http_ctx.json_len = 0;
            s_http_ctx.json_cap = 12288;
            resp_mem[0] = '\0';

            esp_http_client_set_url(client, "https://api.groq.com/openai/v1/chat/completions");
            esp_http_client_set_method(client, HTTP_METHOD_POST);
            esp_http_client_set_header(client, "Authorization", auth_hdr);
            esp_http_client_set_header(client, "Content-Type", "application/json");
            esp_http_client_set_post_field(client, chat_body, chat_body_len);

            err = esp_http_client_perform(client);
            status_code = esp_http_client_get_status_code(client);

            if (err == ESP_OK && status_code == 200 && s_http_ctx.json_len > 10) {
                cJSON* resp_json = cJSON_Parse(resp_mem);
                cJSON* choices = resp_json ? cJSON_GetObjectItem(resp_json, "choices") : nullptr;
                cJSON* c0 = (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) ? cJSON_GetArrayItem(choices, 0) : nullptr;
                cJSON* msg = c0 ? cJSON_GetObjectItem(c0, "message") : nullptr;
                cJSON* content = msg ? cJSON_GetObjectItem(msg, "content") : nullptr;

                if (content && cJSON_IsString(content) && content->valuestring[0] != '\0') {
                    s_groq_warm = true;
                    s_groq_last_use_us = esp_timer_get_time();

                    cJSON* structured = cJSON_Parse(content->valuestring);
                    if (structured) {
                        cJSON* j_rep = cJSON_GetObjectItem(structured, "reply");
                        if (j_rep && cJSON_IsString(j_rep)) {
                            strncpy(out_reply_utf8, j_rep->valuestring, max_reply - 1);
                            out_reply_utf8[max_reply - 1] = '\0';
                        }
                        cJSON* j_acts = cJSON_GetObjectItem(structured, "actions");
                        if (cJSON_IsArray(j_acts)) {
                            int n_acts = cJSON_GetArraySize(j_acts);
                            for (int a = 0; a < n_acts && a < 5; a++) {
                                cJSON* it = cJSON_GetArrayItem(j_acts, a);
                                if (it && cJSON_IsString(it) && strlen(it->valuestring) > 1) {
                                    if (out_actions_joined[0] != '\0' && strlen(out_actions_joined) + 3 < max_actions) {
                                        strncat(out_actions_joined, "; ", max_actions - strlen(out_actions_joined) - 1);
                                    }
                                    if (strlen(out_actions_joined) + 2 < max_actions) {
                                        strncat(out_actions_joined, it->valuestring, max_actions - strlen(out_actions_joined) - 1);
                                    }
                                }
                            }
                        }
                        cJSON_Delete(structured);
                    } else {
                        // Graceful fallback if non-JSON string ever arrives
                        strncpy(out_reply_utf8, content->valuestring, max_reply - 1);
                        out_reply_utf8[max_reply - 1] = '\0';
                    }

                    // Store clean reply in short-term history (never stores action tags!)
                    tars_push_history(raw_transcript, out_reply_utf8[0] ? out_reply_utf8 : "Komut uygulandi.");
                    cJSON_Delete(resp_json);
                    res = TARS_RES_OK;
                    break;
                }
                if (resp_json) cJSON_Delete(resp_json);
            }

            ESP_LOGW(TAG, "Groq JSON Chat attempt %d failed (err=%s, http=%d, len=%u, body='%.120s') -> resetting TLS",
                     attempt + 1, esp_err_to_name(err), status_code, (unsigned)s_http_ctx.json_len, resp_mem);
            tars_reset_groq_client();
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        free(chat_body);
    }

    int64_t t_done = esp_timer_get_time();
    out_stats->llm_ms = (int)((t_done - t_chat_start) / 1000);
    out_stats->total_ms = (int)((t_done - t_pipe_start) / 1000);

    ESP_LOGI(TAG, "=== [TARS-4.0] wav=%uKB | STT=%dms | LLM=%dms | Total=%dms | actions='%s' ===",
             (unsigned)(wav_len / 1024), out_stats->stt_ms, out_stats->llm_ms, out_stats->total_ms, out_actions_joined);

    free(raw_transcript);
    free(resp_mem);
    display_manager_reset_timer();
    return res;
}

extern "C" void app_tars_handle_reply_from_ble(const char* payload) {
    if (!payload) return;
    char* clean_reply = (char*)heap_caps_calloc(1, TARS_TEXT_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!clean_reply) return;
    char action_badge[64] = {0};
    tars_finalize_reply(payload, nullptr, clean_reply, TARS_TEXT_CAP, action_badge, sizeof(action_badge));

    if (s_tars_instance && s_tars_instance->is_open()) {
        s_tars_instance->set_visual_state(TARS_VIS_REPLY, 40);
        s_tars_instance->update_ui_state("HAZIR - ELLER SERBEST", 0x00E676, 0, nullptr, clean_reply, action_badge);
    } else {
        display_manager_turn_on();
        s_wake_pill_dismissed = false;
        s_wake_vis_state = TARS_VIS_REPLY;
        s_wake_countdown_active = false;
        update_wake_pill_ui("TARS // YANIT (Dokun: Kapat)", clean_reply, 0x00E676, true);
    }
    free(clean_reply);
}

// Unified Background Worker Task for both 4.5s Triple-Tap Wake Window & Full Hands-Free App Mode
static void tars_worker_loop(void* pv) {
    ESP_LOGI(TAG, "TARS v4.0 Worker started (Strict JSON Schema + Zero-Drop UI Mailbox + Honest PC Control)");
    tars_load_api_key();

    int16_t* pcm_buf = (int16_t*)heap_caps_malloc(TARS_MAX_PCM_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t* ulaw_wav_buf = (uint8_t*)heap_caps_malloc(TARS_ULAW_HDR_BYTES + TARS_MAX_ULAW_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t* frame_buf = (int16_t*)heap_caps_malloc(TARS_FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t* preroll_ring = (int16_t*)heap_caps_malloc(TARS_PREROLL_FRAMES * TARS_FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char* transcript = (char*)heap_caps_malloc(TARS_TEXT_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char* raw_reply = (char*)heap_caps_malloc(TARS_TEXT_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char* clean_reply = (char*)heap_caps_malloc(TARS_TEXT_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    while (true) {
        if (!s_tars_app_active && !s_tars_wake_window_req) {
            if (s_groq_client && !wifi_manager_is_connected()) {
                tars_reset_groq_client();
            }
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
            continue;
        }

        if (!display_manager_is_on() || wifi_manager_is_ap_active()) {
            s_tars_wake_window_req = false;
            tars_reset_groq_client();
            vTaskDelay(pdMS_TO_TICKS(120));
            continue;
        }

        bool is_wake_window = (!s_tars_app_active && s_tars_wake_window_req);
        s_tars_abort_audio = false;
        s_tars_manual_toggle = false;
        s_wake_pill_dismissed = false;

        // Bring up Home Wi-Fi in background WITHOUT interrupting the mic!
        if (!is_wake_window && !wifi_manager_is_active()) {
            wifi_manager_start_async();
        }

        esp_codec_dev_handle_t mic = bsp_get_shared_mic_codec();
        if (!mic || !pcm_buf || !ulaw_wav_buf || !frame_buf || !preroll_ring || !transcript || !raw_reply || !clean_reply) {
            ESP_LOGE(TAG, "TARS mic or PSRAM buffer unavailable");
            s_tars_wake_window_req = false;
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = 1,
            .channel_mask = 0,
            .sample_rate = TARS_SAMPLE_RATE,
            .mclk_multiple = 0,
        };

        if (!bsp_audio_lock(400)) {
            ESP_LOGW(TAG, "Audio bus busy, waiting...");
            s_tars_wake_window_req = false;
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        }
        if (esp_codec_dev_open(mic, &fs) != ESP_OK) {
            bsp_audio_unlock();
            ESP_LOGW(TAG, "Mic busy, waiting...");
            s_tars_wake_window_req = false;
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        }
        esp_codec_dev_set_in_gain(mic, 30.0);

        if (is_wake_window) {
            s_wake_vis_state = TARS_VIS_LISTEN;
            s_wake_mic_pct = 15;
            s_wake_countdown_active = true;
            s_wake_listen_start_us = esp_timer_get_time();
            update_wake_pill_ui("TARS // DINLIYOR", "Konusabilirsin Eymen...", 0x00E5FF, true);
        } else if (s_tars_instance) {
            s_tars_instance->set_visual_state(TARS_VIS_IDLE, 0);
            s_tars_instance->update_ui_state("HAZIR - ELLER SERBEST",
                                             0x00E676, 0, nullptr, nullptr, nullptr, LV_SYMBOL_AUDIO " Konus");
        }

        // Hands-Free Adaptive VAD Loop (DC-Removed + 192ms Pre-Roll + Smart Dynamic Silence Cutoff)
        bool speech_started = false;
        bool manual_rec_mode = false;
        int loud_frames = 0;
        int silent_frames = 0;
        int preroll_idx = 0;
        int preroll_filled = 0;
        size_t samples_written = 0;
        const size_t max_samples = TARS_SAMPLE_RATE * TARS_MAX_REC_SEC;
        uint32_t noise_floor = 260;
        uint32_t max_speech_energy = 0;
        TickType_t listen_start_tick = xTaskGetTickCount();
        TickType_t speech_start_tick = 0;

        for (int w = 0; w < 2; w++) {
            esp_codec_dev_read(mic, frame_buf, TARS_FRAME_BYTES);
        }

        while ((s_tars_app_active || s_tars_wake_window_req) && display_manager_is_on() && !s_tars_abort_audio && !s_wake_pill_dismissed) {
            if (esp_codec_dev_read(mic, frame_buf, TARS_FRAME_BYTES) != 0) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }

            int64_t dc_sum = 0;
            for (int i = 0; i < TARS_FRAME_SAMPLES; i++) dc_sum += frame_buf[i];
            int32_t dc_avg = (int32_t)(dc_sum / TARS_FRAME_SAMPLES);

            uint64_t sum_abs = 0;
            for (int i = 0; i < TARS_FRAME_SAMPLES; i++) {
                int32_t v = (int32_t)frame_buf[i] - dc_avg;
                sum_abs += (v < 0) ? -v : v;
            }
            uint32_t mean_abs = (uint32_t)(sum_abs / TARS_FRAME_SAMPLES);
            int level_pct = (int)((mean_abs * 100) / 3000);
            if (level_pct > 100) level_pct = 100;
            if (is_wake_window) {
                s_wake_mic_pct = level_pct;
            }

            if (!speech_started) {
                memcpy(preroll_ring + (preroll_idx * TARS_FRAME_SAMPLES), frame_buf, TARS_FRAME_BYTES);
                preroll_idx = (preroll_idx + 1) % TARS_PREROLL_FRAMES;
                if (preroll_filled < TARS_PREROLL_FRAMES) preroll_filled++;

                noise_floor = (noise_floor * 7 + mean_abs) / 8;
                uint32_t trig_thresh = noise_floor * 2 + 240;
                if (trig_thresh < 460) trig_thresh = 460;
                if (trig_thresh > 2200) trig_thresh = 2200;

                bool manual_pressed = s_tars_manual_toggle;
                if (manual_pressed) {
                    s_tars_manual_toggle = false;
                    manual_rec_mode = true;
                }

                if (mean_abs > trig_thresh || manual_pressed) {
                    loud_frames++;
                    if (loud_frames >= 3 || manual_pressed) {
                        speech_started = true;
                        speech_start_tick = xTaskGetTickCount();
                        silent_frames = 0;
                        max_speech_energy = mean_abs;
                        display_manager_reset_timer();

                        samples_written = 0;
                        int start_slot = (preroll_filled < TARS_PREROLL_FRAMES) ? 0 : preroll_idx;
                        for (int f = 0; f < preroll_filled; f++) {
                            int slot = (start_slot + f) % TARS_PREROLL_FRAMES;
                            memcpy(pcm_buf + samples_written, preroll_ring + (slot * TARS_FRAME_SAMPLES), TARS_FRAME_BYTES);
                            samples_written += TARS_FRAME_SAMPLES;
                        }

                        if (!wifi_manager_is_active() && !wifi_manager_is_ap_active()) {
                            wifi_manager_start_async();
                        } else if (!wifi_manager_is_ap_active() && (wifi_manager_get_state() == WIFI_STATE_IDLE || wifi_manager_get_state() == WIFI_STATE_FAILED)) {
                            wifi_manager_connect_default_async();
                        }

                        if (is_wake_window) {
                            s_wake_vis_state = TARS_VIS_LISTEN;
                            s_wake_countdown_active = false;
                            update_wake_pill_ui("TARS // SENI DINLIYOR", "Konusmaya devam edebilirsin...", 0x00E676, true);
                        } else if (s_tars_instance) {
                            s_tars_instance->set_visual_state(TARS_VIS_LISTEN, level_pct);
                            s_tars_instance->update_ui_state("DINLIYOR...", 0xFFA000, level_pct, "Dinleniyor...", nullptr, nullptr, LV_SYMBOL_OK " Gonder");
                        }
                    }
                } else {
                    loud_frames = 0;
                }

                uint32_t waited_ms = (xTaskGetTickCount() - listen_start_tick) * portTICK_PERIOD_MS;
                if (is_wake_window && waited_ms >= 4500) {
                    s_tars_wake_window_req = false;
                    break;
                }
            } else {
                display_manager_reset_timer();
                if (samples_written + TARS_FRAME_SAMPLES <= max_samples) {
                    memcpy(pcm_buf + samples_written, frame_buf, TARS_FRAME_BYTES);
                    samples_written += TARS_FRAME_SAMPLES;
                }
                if (mean_abs > max_speech_energy) max_speech_energy = mean_abs;

                uint32_t silence_thresh = noise_floor + 220;
                if (silence_thresh < 340) silence_thresh = 340;
                if (mean_abs < silence_thresh) {
                    silent_frames++;
                } else {
                    silent_frames = 0;
                }

                uint32_t rec_ms = (xTaskGetTickCount() - speech_start_tick) * portTICK_PERIOD_MS;
                if (!is_wake_window && s_tars_instance) {
                    s_tars_instance->set_visual_state(TARS_VIS_LISTEN, level_pct);
                    if ((samples_written / TARS_FRAME_SAMPLES) % 6 == 0) {
                        char rec_hdr[48];
                        snprintf(rec_hdr, sizeof(rec_hdr), "DINLIYOR (%.1f sn)...", (float)rec_ms / 1000.0f);
                        s_tars_instance->update_ui_state(rec_hdr, 0xFFA000, level_pct, nullptr, nullptr, nullptr);
                    }
                }

                int stop_silence_frames = manual_rec_mode ? 50 : ((rec_ms < 1400) ? 34 : (rec_ms < 3500) ? 24 : 19);
                bool manual_finish = s_tars_manual_toggle;
                if (manual_finish) s_tars_manual_toggle = false;

                if (silent_frames >= stop_silence_frames || samples_written >= max_samples || manual_finish) {
                    break;
                }
            }
        }

        // Release mic & I2S cleanly
        esp_codec_dev_close(mic);
        bsp_audio_standby();
        bsp_audio_unlock();

        size_t trail_trim_samples = (silent_frames > 9) ? (size_t)((silent_frames - 7) * TARS_FRAME_SAMPLES) : 0;
        if (trail_trim_samples > 0 && samples_written > trail_trim_samples + 4096) {
            samples_written -= trail_trim_samples;
        }

        if (!speech_started || s_tars_abort_audio || s_wake_pill_dismissed || samples_written < 4096 || max_speech_energy < 320) {
            if (is_wake_window) {
                s_tars_wake_window_req = false;
                update_wake_pill_ui(nullptr, nullptr, 0, false);
            }
            continue;
        }

        size_t ulaw_wav_len = tars_encode_ulaw_wav(pcm_buf, samples_written, ulaw_wav_buf);

        transcript[0] = '\0';
        raw_reply[0] = '\0';
        clean_reply[0] = '\0';
        char structured_actions[256] = {0};
        char action_badge[64] = {0};
        TarsTimingStats stats = {};

        TarsPipelineResult res = tars_run_groq_turbo_pipeline(
            ulaw_wav_buf, ulaw_wav_len, is_wake_window,
            transcript, TARS_TEXT_CAP,
            raw_reply, TARS_TEXT_CAP,
            structured_actions, sizeof(structured_actions),
            &stats
        );

        if (res == TARS_RES_SILENCE_HALLUCINATION) {
            wifi_manager_set_turbo(false);
            if (is_wake_window) {
                s_tars_wake_window_req = false;
                update_wake_pill_ui("TARS", "Anlasilabilir bir konusma duyulmamis gibi. Tekrar deneyebilirsin.", 0x9E9E9E, true);
                for (int t = 0; t < 5 && !s_wake_pill_dismissed && display_manager_is_on(); t++) {
                    display_manager_reset_timer();
                    vTaskDelay(pdMS_TO_TICKS(450));
                }
                update_wake_pill_ui(nullptr, nullptr, 0, false);
            } else if (s_tars_instance) {
                s_tars_instance->set_visual_state(TARS_VIS_IDLE, 0);
                s_tars_instance->update_ui_state("ANLASILMADI - TEKRAR KONUS", 0x9E9E9E, 0, nullptr, nullptr, "EYLEM: YOK", LV_SYMBOL_AUDIO " Konus");
                vTaskDelay(pdMS_TO_TICKS(350));
            }
            continue;
        }

        if (res != TARS_RES_OK) {
            const char* reason = (res == TARS_RES_NO_WIFI)
                ? "Ev Wi-Fi agina baglanilamadi. Wi-Fi kapsama alanini kontrol et."
                : "Bulut yanit vermedi (zaman asimi). Lutfen tekrar soyle.";
            if (strlen(transcript) == 0) {
                strncpy(transcript, "(Cevrimdisi Mod)", TARS_TEXT_CAP - 1);
            }
            tars_local_turkish_fallback(transcript, reason, raw_reply, TARS_TEXT_CAP);
            structured_actions[0] = '\0';
        }

        // Execute PC/TV/Home actions BEFORE dropping Wi-Fi turbo mode!
        tars_finalize_reply(raw_reply, structured_actions, clean_reply, TARS_TEXT_CAP, action_badge, sizeof(action_badge), transcript);
        wifi_manager_set_turbo(false);

        char speed_hdr[64];
        if (res == TARS_RES_OK) {
            snprintf(speed_hdr, sizeof(speed_hdr), "TARS 4.0 // %dms (STT:%d AI:%d)",
                     stats.total_ms, stats.stt_ms, stats.llm_ms);
        } else {
            snprintf(speed_hdr, sizeof(speed_hdr), "TARS // YANIT (Dokun: Kapat)");
        }

        if (is_wake_window) {
            s_tars_wake_window_req = false;
            s_wake_pill_dismissed = false;
            s_wake_vis_state = TARS_VIS_REPLY;
            s_wake_countdown_active = false;
            uint32_t card_col = (res == TARS_RES_OK) ? 0x00E676 : 0xFFA000;
            update_wake_pill_ui(speed_hdr, clean_reply, card_col, true);
            for (int t = 0; t < 13 && !s_wake_pill_dismissed && !s_tars_app_active && display_manager_is_on(); t++) {
                display_manager_reset_timer();
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            update_wake_pill_ui(nullptr, nullptr, 0, false);
        } else if (s_tars_instance) {
            char app_status[64];
            if (res == TARS_RES_OK) {
                snprintf(app_status, sizeof(app_status), "HIZ: %dms (STT:%d | AI:%d)",
                         stats.total_ms, stats.stt_ms, stats.llm_ms);
            } else {
                snprintf(app_status, sizeof(app_status), "HAZIR - ELLER SERBEST");
            }
            s_tars_instance->set_visual_state(TARS_VIS_REPLY, 40);
            s_tars_instance->update_ui_state(app_status, 0x00E676, 0, transcript, clean_reply, action_badge, LV_SYMBOL_AUDIO " Konus");
            vTaskDelay(pdMS_TO_TICKS(450));
        }
    }
}

static void ensure_tars_worker(void) {
    if (s_tars_task == nullptr) {
        // 16KB INTERNAL SRAM stack for rock-solid NVS + Wi-Fi + Persistent mbedTLS + chained inline SSE actions
        xTaskCreatePinnedToCoreWithCaps(
            tars_worker_loop,
            "tars_wrk",
            16384,
            nullptr,
            4,
            &s_tars_task,
            1,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
        );
    }
}

extern "C" void app_tars_on_triple_tap_wake(void) {
    if (wifi_manager_is_ap_active()) return;
    ensure_tars_worker();
    if (!s_tars_app_active) {
        s_tars_wake_window_req = true;
        if (s_tars_task) xTaskNotifyGive(s_tars_task);
    }
}

static void on_tars_back_clicked(lv_event_t* e) {
    AppTars* app = (AppTars*)lv_event_get_user_data(e);
    if (app) {
        lv_async_call([](void* data) {
            AppTars* a = (AppTars*)data;
            if (a) a->request_close();
        }, app);
    }
}

// ============================================================================
// 1-BIT PIXEL TARS MONOLITH LIVE AMOLED ENGINE (44x24 grid @ P=8 -> 352x192)
// Stored 100% in Octal PSRAM (132 KB) - ZERO Internal SRAM overhead!
// ============================================================================
static constexpr int TARS_MONO_GW = 44;
static constexpr int TARS_MONO_GH = 24;
static constexpr int TARS_MONO_P  = 8;
static constexpr int TARS_MONO_W  = TARS_MONO_GW * TARS_MONO_P; // 352
static constexpr int TARS_MONO_H  = TARS_MONO_GH * TARS_MONO_P; // 192

static uint16_t* s_monolith_buf = nullptr;
static lv_image_dsc_t s_monolith_dsc = {
    .header = {
        .magic = LV_IMAGE_HEADER_MAGIC,
        .cf = LV_COLOR_FORMAT_RGB565,
        .flags = 0,
        .w = TARS_MONO_W,
        .h = TARS_MONO_H,
        .stride = TARS_MONO_W * 2,
        .reserved_2 = 0
    },
    .data_size = TARS_MONO_W * TARS_MONO_H * 2,
    .data = nullptr,
    .reserved = NULL
};

static inline void mono_px(uint16_t* buf, int gx, int gy, int gw, int gh, uint16_t col) {
    mono_px_s(buf, TARS_MONO_W, TARS_MONO_H, TARS_MONO_P, gx, gy, gw, gh, col);
}

static void mono_draw_tars_word(uint16_t* buf, int gx, int gy, uint16_t col) {
    mono_draw_tars_word_s(buf, TARS_MONO_W, TARS_MONO_H, TARS_MONO_P, gx, gy, col);
}

static void mono_draw_z(uint16_t* buf, int gx, int gy, uint16_t col) {
    mono_px(buf, gx, gy, 3, 1, col);
    mono_px(buf, gx + 2, gy + 1, 1, 1, col);
    mono_px(buf, gx + 1, gy + 2, 1, 1, col);
    mono_px(buf, gx, gy + 3, 1, 1, col);
    mono_px(buf, gx, gy + 4, 3, 1, col);
}

// Zero-Drop Cross-Thread UI Mailbox (background worker NEVER blocks on bsp_display_lock!)
struct TarsUiMailbox {
    bool has_status;
    char status[72];
    uint32_t status_color;
    bool has_user;
    char user[256];
    bool has_tars;
    char tars[512];
    bool has_action;
    char action[96];
    bool has_btn;
    char btn[40];
};
static portMUX_TYPE s_ui_mux = portMUX_INITIALIZER_UNLOCKED;
static TarsUiMailbox s_ui_mb = {};

static void on_tars_anim_timer(lv_timer_t* timer) {
    AppTars* app = (AppTars*)lv_timer_get_user_data(timer);
    if (app && app->is_open()) {
        app->render_monolith_frame();
    }
}

AppTars* AppTars::get_instance() {
    return s_tars_instance;
}

AppTars::AppTars()
    : esp_brookesia::systems::phone::App("TARS", &icon_voice_recorder, true),
      _bg_obj(nullptr), _btn_back(nullptr), _title_label(nullptr), _params_label(nullptr),
      _btn_tone(nullptr), _btn_tone_label(nullptr), _monolith_img(nullptr), _status_label(nullptr),
      _dialog_card(nullptr), _user_label(nullptr), _tars_label(nullptr), _action_badge(nullptr),
      _btn_turbo(nullptr), _btn_turbo_label(nullptr), _btn_humor(nullptr), _btn_humor_label(nullptr),
      _btn_test(nullptr), _btn_test_label(nullptr), _anim_timer(nullptr),
      _humor_pct(75), _honesty_pct(90), _monolith_tone(0), _monolith_style(0),
      _turbo_mode(false), _vis_state(TARS_VIS_IDLE), _live_mic_pct(0),
      _reply_anim_until_us(0), _cached_bat_pct(100), _cached_charging(false),
      _last_bat_check_us(0), _anim_frame(0), _idle_skip_div(0) {
    s_tars_instance = this;
}

AppTars::~AppTars() {
    if (s_tars_instance == this) {
        s_tars_instance = nullptr;
    }
}

void AppTars::load_ui_prefs() {
    nvs_handle_t h;
    if (nvs_open("tars_cfg", NVS_READONLY, &h) == ESP_OK) {
        int32_t v = 0;
        if (nvs_get_i32(h, "ui_tone", &v) == ESP_OK && v >= 0 && v < 4) _monolith_tone = (int)v;
        if (nvs_get_i32(h, "ui_style", &v) == ESP_OK && v >= 0 && v < 3) _monolith_style = (int)v;
        if (nvs_get_i32(h, "humor_pct", &v) == ESP_OK && v >= 0 && v <= 100) _humor_pct = (int)v;
        nvs_close(h);
    }
}

void AppTars::save_ui_prefs() {
    s_shared_ui_tone = _monolith_tone;
    s_shared_ui_style = _monolith_style;
    s_shared_prefs_loaded = true;
    nvs_handle_t h;
    if (nvs_open("tars_cfg", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "ui_tone", _monolith_tone);
        nvs_set_i32(h, "ui_style", _monolith_style);
        nvs_set_i32(h, "humor_pct", _humor_pct);
        nvs_commit(h);
        nvs_close(h);
    }
}

void AppTars::set_visual_state(TarsVisState state, int mic_level_pct) {
    _vis_state = (int)state;
    if (mic_level_pct >= 0) {
        _live_mic_pct = (mic_level_pct > 100) ? 100 : mic_level_pct;
    }
    if (state == TARS_VIS_REPLY) {
        _reply_anim_until_us = esp_timer_get_time() + 2600000LL;
    }
}

void AppTars::set_turbo_mode(bool enabled) {
    _turbo_mode = enabled;
}

void AppTars::cycle_tone() {
    _monolith_tone = (_monolith_tone + 1) % 4;
    save_ui_prefs();
    apply_theme_chrome();
    _idle_skip_div = 0;
    render_monolith_frame();
    bsp_vibrate_pulse(25);
}

void AppTars::cycle_style() {
    _monolith_style = (_monolith_style + 1) % 3;
    save_ui_prefs();
    apply_theme_chrome();
    _idle_skip_div = 0;
    render_monolith_frame();
    bsp_vibrate_pulse(25);
}

void AppTars::apply_theme_chrome() {
    if (!_bg_obj) return;
    bool low_bat = (!_turbo_mode && _cached_bat_pct > 0 && _cached_bat_pct < 15 && !_cached_charging);
    const TarsMonoPalette& pal = _turbo_mode ? kTarsTurboPal : (low_bat ? kTarsLowBatPal : kTarsPalettes[_monolith_tone]);

    if (_title_label) {
        lv_label_set_text(_title_label, _turbo_mode ? "TARS // TURBO" : "TARS");
        lv_obj_set_style_text_color(_title_label, lv_color_hex(pal.fg), 0);
    }
    if (_params_label) {
        lv_label_set_text_fmt(_params_label, "%s | MIZAH %%%d | PIL %%%d",
                              kTarsStyleNames[_monolith_style], _humor_pct, _cached_bat_pct);
        lv_obj_set_style_text_color(_params_label, lv_color_hex(pal.mid), 0);
    }
    if (_btn_tone) {
        lv_obj_set_style_border_color(_btn_tone, lv_color_hex(pal.mid), 0);
        if (_btn_tone_label) {
            lv_label_set_text(_btn_tone_label, kTarsPalettes[_monolith_tone].name);
            lv_obj_set_style_text_color(_btn_tone_label, lv_color_hex(pal.fg), 0);
        }
    }
    if (_monolith_img) {
        lv_obj_set_style_border_color(_monolith_img, lv_color_hex(pal.mid), 0);
    }
    if (_dialog_card) {
        lv_obj_set_style_border_color(_dialog_card, lv_color_hex(pal.mid), 0);
    }
    if (_action_badge) {
        lv_obj_set_style_text_color(_action_badge, lv_color_hex(pal.acc), 0);
    }
    if (_btn_turbo) {
        lv_obj_set_style_bg_color(_btn_turbo, lv_color_hex(_turbo_mode ? 0xB91C1C : 0x1C222D), 0);
        lv_obj_set_style_border_color(_btn_turbo, lv_color_hex(_turbo_mode ? 0xF87171 : 0x334155), 0);
        if (_btn_turbo_label) {
            lv_obj_set_style_text_color(_btn_turbo_label, lv_color_hex(_turbo_mode ? 0xFFFFFF : 0xCBD5E1), 0);
        }
    }
    if (_btn_humor_label) {
        lv_label_set_text_fmt(_btn_humor_label, "M:%%%d", _humor_pct);
    }
}

void AppTars::drain_ui_mailbox() {
    if (!_bg_obj) return;
    TarsUiMailbox local = {};
    portENTER_CRITICAL(&s_ui_mux);
    if (s_ui_mb.has_status || s_ui_mb.has_user || s_ui_mb.has_tars || s_ui_mb.has_action || s_ui_mb.has_btn) {
        local = s_ui_mb;
        s_ui_mb.has_status = false;
        s_ui_mb.has_user = false;
        s_ui_mb.has_tars = false;
        s_ui_mb.has_action = false;
        s_ui_mb.has_btn = false;
    }
    portEXIT_CRITICAL(&s_ui_mux);

    if (local.has_status && _status_label) {
        lv_label_set_text(_status_label, local.status);
        lv_obj_set_style_text_color(_status_label, lv_color_hex(local.status_color), 0);
    }
    if (local.has_user && _user_label) {
        lv_label_set_text_fmt(_user_label, "> SEN: \"%s\"", local.user);
    }
    if (local.has_tars && _tars_label) {
        lv_label_set_text_fmt(_tars_label, "TARS: \"%s\"", local.tars);
        if (_dialog_card) {
            lv_obj_scroll_to_y(_dialog_card, 0, LV_ANIM_OFF);
        }
    }
    if (local.has_action && _action_badge) {
        lv_label_set_text(_action_badge, local.action);
    }
    if (local.has_btn && _btn_test_label) {
        lv_label_set_text(_btn_test_label, local.btn);
    }
}

void AppTars::render_monolith_frame() {
    if (!_bg_obj || !_monolith_img || !s_monolith_buf) return;

    // ALWAYS drain pending UI text/status updates FIRST on the LVGL thread (zero drops, zero lock contention!)
    drain_ui_mailbox();

    if (!display_manager_is_on()) return;

    int64_t now_us = esp_timer_get_time();
    static bool s_prev_turbo = false;
    static bool s_prev_lowbat = false;

    // Refresh battery every 5 seconds (lightweight PMIC read)
    if (now_us - _last_bat_check_us > 5000000LL) {
        _last_bat_check_us = now_us;
        int pct = bsp_battery_get_percent();
        if (pct >= 0 && pct <= 100) _cached_bat_pct = pct;
        _cached_charging = bsp_battery_is_charging();
        if (_params_label) {
            lv_label_set_text_fmt(_params_label, "%s | MIZAH %%%d | PIL %%%d",
                                  kTarsStyleNames[_monolith_style], _humor_pct, _cached_bat_pct);
        }
    }

    int vis = _vis_state;
    if (vis == TARS_VIS_REPLY && now_us > _reply_anim_until_us) {
        vis = TARS_VIS_IDLE;
        _vis_state = TARS_VIS_IDLE;
    }

    bool turbo = _turbo_mode;
    bool low_bat = (!turbo && _cached_bat_pct > 0 && _cached_bat_pct < 15 && !_cached_charging &&
                    vis != TARS_VIS_LISTEN && vis != TARS_VIS_THINK);

    // Check for tone/style/turbo change
    if (turbo != s_prev_turbo || low_bat != s_prev_lowbat) {
        s_prev_turbo = turbo;
        s_prev_lowbat = low_bat;
        _idle_skip_div = 0;
        apply_theme_chrome();
    }

    // Smart Adaptive FPS: 12.5 FPS (80ms) when active/Turbo; ~4 FPS (240ms) during quiet Idle/LowBat
    // Slashes continuous PSRAM/QSPI DMA load by 75% so Wi-Fi, BLE, and Touch stay ultra-snappy!
    if (vis == TARS_VIS_IDLE && !turbo) {
        if ((_idle_skip_div++ % 3) != 0) {
            return;
        }
    } else {
        _idle_skip_div = 0;
    }

    const TarsMonoPalette& pal = turbo ? kTarsTurboPal : (low_bat ? kTarsLowBatPal : kTarsPalettes[_monolith_tone]);
    const uint16_t c_black = 0x0000;
    const uint16_t c_white = 0xFFFF;
    const uint16_t c_fg    = tars_hex565(pal.fg);
    const uint16_t c_mid   = tars_hex565(pal.mid);
    const uint16_t c_dim   = tars_hex565(pal.dim);
    const uint16_t c_acc   = tars_hex565(pal.acc);

    // Clear 352x192 PSRAM RGB565 buffer to pure pitch-black AMOLED
    memset(s_monolith_buf, 0, TARS_MONO_W * TARS_MONO_H * sizeof(uint16_t));

    float t = (float)(now_us / 1000LL) * 0.001f; // Wall-clock seconds (smooth regardless of adaptive FPS!)
    _anim_frame++;
    float energy = (float)_live_mic_pct / 100.0f;
    if (vis == TARS_VIS_LISTEN && energy < 0.15f) energy = 0.15f;

    bool is_asym = (_monolith_style == 2) && !turbo;
    const int widths[4] = {
        is_asym ? 8 : 7,
        8,
        is_asym ? 7 : 8,
        7
    };
    int sxList[4];
    int curX = 5;
    for (int i = 0; i < 4; i++) {
        sxList[i] = curX;
        curX += widths[i] + 1;
    }

    const int baseY = 2;
    const int slabH = 20;

    // 1. Draw 4 Articulated Brushed-Steel Pixel Slabs
    for (int i = 0; i < 4; i++) {
        int yShift = 0;
        if (vis == TARS_VIS_LISTEN) {
            int amp = (int)lroundf(energy * 2.2f);
            if (amp > 2) amp = 2;
            yShift = (i == 0 || i == 3) ? -amp : (int)lroundf(sinf(t * 12.0f + (float)i) * (float)amp * 0.5f);
        } else if (vis == TARS_VIS_THINK) {
            yShift = (int)lroundf(sinf(t * 5.5f + (float)i * 1.4f) * 2.0f);
        } else if (turbo) {
            yShift = (((int)(t * 14.0f) + i) % 2 == 0) ? -1 : 1;
        }

        int sx = sxList[i];
        int sw = widths[i];
        int sy = baseY + yShift;
        if (sy < 1) sy = 1;
        if (sy + slabH > TARS_MONO_GH - 1) sy = (TARS_MONO_GH - 1) - slabH;

        mono_px(s_monolith_buf, sx, sy, sw, slabH, c_fg);
        mono_px(s_monolith_buf, sx + sw - 1, sy, 1, slabH, c_mid);           // right metallic bevel
        mono_px(s_monolith_buf, sx + 1, sy, sw - 3, 1, c_white);             // top machined edge
        mono_px(s_monolith_buf, sx, sy + 14, sw, 1, c_black);                // lower articulation seam

        uint16_t dotCol = turbo ? c_acc : c_black;
        if (is_asym) {
            if (i == 2) {
                mono_px(s_monolith_buf, sx + 2, sy + 4, 3, 1, c_black);
                mono_px(s_monolith_buf, sx + 2, sy + 6, 3, 1, c_black);
                mono_px(s_monolith_buf, sx + 2, sy + 8, 3, 1, c_black);
                mono_px(s_monolith_buf, sx + 2, sy + 11, 2, 1, low_bat ? c_dim : c_acc);
            } else if (i == 3) {
                mono_px(s_monolith_buf, sx + 2, sy + 4, 1, 1, dotCol);
                mono_px(s_monolith_buf, sx + 4, sy + 4, 1, 1, dotCol);
                mono_px(s_monolith_buf, sx + 2, sy + 7, 1, 1, dotCol);
                mono_px(s_monolith_buf, sx + 4, sy + 10, 1, 1, dotCol);
            }
        } else if (i == 0 || i == 3) {
            mono_px(s_monolith_buf, sx + 2, sy + 4, 1, 1, dotCol);
            mono_px(s_monolith_buf, sx + 4, sy + 4, 1, 1, dotCol);
            mono_px(s_monolith_buf, sx + 2, sy + 7, 1, 1, dotCol);
            mono_px(s_monolith_buf, sx + 4, sy + 10, 1, 1, dotCol);
        }

        // Lower grip slots
        mono_px(s_monolith_buf, sx + 2, sy + 16, 1, 3, c_mid);
        mono_px(s_monolith_buf, sx + sw - 3, sy + 16, 1, 3, c_mid);
    }

    // 2. Glowing Crimson Vertical Joint Seams in Oyun / Turbo Mode
    if (turbo) {
        for (int g = 0; g < 3; g++) {
            mono_px(s_monolith_buf, sxList[g] + widths[g], baseY + 2, 1, slabH - 4, c_acc);
        }
    }

    // 3. Recessed Chest LCD Screen (17x10 pixels)
    const int lx = is_asym ? sxList[0] : sxList[1];
    const int ly = 5;
    const int lw = 17;
    const int lh = 10;

    mono_px(s_monolith_buf, lx, ly, lw, lh, c_black);
    mono_px(s_monolith_buf, lx, ly, lw, 1, c_mid); // top recessed bezel

    if (vis == TARS_VIS_LISTEN) {
        // Real-time 7-bar pixel voice equalizer driven by live ES7210 mic!
        for (int b = 0; b < 7; b++) {
            float wave = 0.45f + 0.55f * fabsf(sinf(t * 13.0f + (float)b * 0.9f));
            int bh = (int)lroundf(energy * 7.8f * wave);
            if (bh < 1) bh = 1;
            if (bh > 7) bh = 7;
            mono_px(s_monolith_buf, lx + 2 + b * 2, ly + lh - 1 - bh, 1, bh, (bh >= 5) ? c_acc : c_fg);
        }
    } else if (vis == TARS_VIS_THINK) {
        // Rotating 1-bit pixel spinner
        int step = ((int)(t * 8.0f)) % 4;
        int cx = lx + lw / 2;
        int cy = ly + lh / 2;
        mono_px(s_monolith_buf, cx, cy, 1, 1, c_acc);
        if (step == 0) {
            mono_px(s_monolith_buf, cx, cy - 2, 1, 2, c_fg);
            mono_px(s_monolith_buf, cx, cy + 1, 1, 2, c_fg);
        } else if (step == 1) {
            mono_px(s_monolith_buf, cx + 1, cy - 1, 1, 1, c_fg);
            mono_px(s_monolith_buf, cx + 2, cy - 2, 1, 1, c_fg);
            mono_px(s_monolith_buf, cx - 1, cy + 1, 1, 1, c_fg);
            mono_px(s_monolith_buf, cx - 2, cy + 2, 1, 1, c_fg);
        } else if (step == 2) {
            mono_px(s_monolith_buf, cx - 2, cy, 2, 1, c_fg);
            mono_px(s_monolith_buf, cx + 1, cy, 2, 1, c_fg);
        } else {
            mono_px(s_monolith_buf, cx - 1, cy - 1, 1, 1, c_fg);
            mono_px(s_monolith_buf, cx - 2, cy - 2, 1, 1, c_fg);
            mono_px(s_monolith_buf, cx + 1, cy + 1, 1, 1, c_fg);
            mono_px(s_monolith_buf, cx + 2, cy + 2, 1, 1, c_fg);
        }
    } else if (turbo) {
        // EXACT Oyun / Turbo slanted red visor eyes + Knight-Rider scan bar!
        int mid = lx + lw / 2;
        mono_px(s_monolith_buf, mid - 6, ly + 2, 2, 1, c_acc);
        mono_px(s_monolith_buf, mid - 5, ly + 3, 3, 2, c_fg);
        mono_px(s_monolith_buf, mid + 4, ly + 2, 2, 1, c_acc);
        mono_px(s_monolith_buf, mid + 2, ly + 3, 3, 2, c_fg);
        int span = lw - 6; // 11
        int raw = ((int)(t * 16.0f)) % (span * 2);
        int phase = (raw < span) ? raw : (span * 2 - raw);
        mono_px(s_monolith_buf, lx + 2 + phase, ly + lh - 2, 3, 1, c_acc);
    } else if (low_bat) {
        // Blinking low-battery pixel icon + floating pixel Z
        int bx = lx + 4;
        int by = ly + 3;
        mono_px(s_monolith_buf, bx, by, 8, 5, c_mid);
        mono_px(s_monolith_buf, bx + 1, by + 1, 6, 3, c_black);
        mono_px(s_monolith_buf, bx + 8, by + 1, 1, 3, c_mid);
        if (sinf(t * 3.0f) > 0.0f) {
            mono_px(s_monolith_buf, bx + 2, by + 2, 1, 1, c_acc);
        }
        int zf = ((int)(t * 2.0f)) % 2;
        mono_draw_z(s_monolith_buf, 39, 1 + zf, c_acc);
    } else if (vis == TARS_VIS_REPLY) {
        // Crisp "TARS" + subtle talking waveform strip underneath
        mono_draw_tars_word(s_monolith_buf, lx + 1, ly + 1, c_fg);
        for (int b = 0; b < 7; b++) {
            int bh = (sinf(t * 14.0f + (float)b * 1.6f) > 0.1f) ? 2 : 1;
            mono_px(s_monolith_buf, lx + 2 + b * 2, ly + lh - bh, 1, bh, c_acc);
        }
    } else {
        // === AUTHENTIC FACELESS INTERSTELLAR IDLE ("TARS") ===
        mono_draw_tars_word(s_monolith_buf, lx + 1, ly + 2, c_fg);
        if (_monolith_style == 0) {
            // Saf Film TARS: slow scanning hardware track + blinking terminal dot
            mono_px(s_monolith_buf, lx + 1, ly + lh - 2, lw - 2, 1, c_dim);
            int sweep = ((int)(t * 4.0f)) % (lw - 4);
            mono_px(s_monolith_buf, lx + 1 + sweep, ly + lh - 2, 3, 1, c_acc);
        } else if (_monolith_style == 1) {
            // NASA Telemetri: live Humor % bar + pulsing readiness bar
            mono_px(s_monolith_buf, lx + 1, ly + 8, lw - 2, 1, c_dim);
            int hbar = (_humor_pct * (lw - 2)) / 100;
            if (hbar < 1) hbar = 1;
            mono_px(s_monolith_buf, lx + 1, ly + 8, hbar, 1, c_acc);
            int pulse = 4 + (int)lroundf((sinf(t * 2.2f) + 1.0f) * 3.5f);
            if (pulse > lw - 2) pulse = lw - 2;
            mono_px(s_monolith_buf, lx + 1, ly + 9, pulse, 1, c_mid);
        } else {
            // Asimetrik Orijinal: blinking block cursor + calm telemetry rail
            if (((int)(t * 2.2f)) % 2 == 0) {
                mono_px(s_monolith_buf, lx + 1, ly + 8, 2, 1, c_acc);
            }
            mono_px(s_monolith_buf, lx + 4, ly + 8, lw - 6, 1, c_mid);
        }
    }

    lv_obj_invalidate(_monolith_img);
}

void AppTars::set_humor(int h) {
    _humor_pct = h;
    save_ui_prefs();
    if (_bg_obj && bsp_display_lock(100)) {
        if (_bg_obj) {
            apply_theme_chrome();
        }
        bsp_display_unlock();
    }
}

void AppTars::update_ui_state(const char* status_text, uint32_t status_color, int mic_level_pct,
                              const char* user_text, const char* tars_text, const char* action_text,
                              const char* trigger_btn_text) {
    if (mic_level_pct >= 0) {
        _live_mic_pct = (mic_level_pct > 100) ? 100 : mic_level_pct;
    }
    // Non-blocking <5us write into UI Mailbox (LVGL timer drains on UI thread -> ZERO dropped replies!)
    portENTER_CRITICAL(&s_ui_mux);
    if (status_text) {
        strncpy(s_ui_mb.status, status_text, sizeof(s_ui_mb.status) - 1);
        s_ui_mb.status[sizeof(s_ui_mb.status) - 1] = '\0';
        s_ui_mb.status_color = status_color;
        s_ui_mb.has_status = true;
    }
    if (user_text) {
        strncpy(s_ui_mb.user, user_text, sizeof(s_ui_mb.user) - 1);
        s_ui_mb.user[sizeof(s_ui_mb.user) - 1] = '\0';
        s_ui_mb.has_user = true;
    }
    if (tars_text) {
        strncpy(s_ui_mb.tars, tars_text, sizeof(s_ui_mb.tars) - 1);
        s_ui_mb.tars[sizeof(s_ui_mb.tars) - 1] = '\0';
        s_ui_mb.has_tars = true;
    }
    if (action_text) {
        strncpy(s_ui_mb.action, action_text, sizeof(s_ui_mb.action) - 1);
        s_ui_mb.action[sizeof(s_ui_mb.action) - 1] = '\0';
        s_ui_mb.has_action = true;
    }
    if (trigger_btn_text) {
        strncpy(s_ui_mb.btn, trigger_btn_text, sizeof(s_ui_mb.btn) - 1);
        s_ui_mb.btn[sizeof(s_ui_mb.btn) - 1] = '\0';
        s_ui_mb.has_btn = true;
    }
    portEXIT_CRITICAL(&s_ui_mux);
}

bool AppTars::run() {
    if (_bg_obj != nullptr) return true;

    portENTER_CRITICAL(&s_ui_mux);
    memset(&s_ui_mb, 0, sizeof(s_ui_mb));
    portEXIT_CRITICAL(&s_ui_mux);
    _idle_skip_div = 0;

    load_ui_prefs();
    _cached_bat_pct = bsp_battery_get_percent();
    if (_cached_bat_pct < 0 || _cached_bat_pct > 100) _cached_bat_pct = 100;
    _cached_charging = bsp_battery_is_charging();
    _last_bat_check_us = esp_timer_get_time();

    if (!s_monolith_buf) {
        s_monolith_buf = (uint16_t*)heap_caps_calloc(TARS_MONO_W * TARS_MONO_H, sizeof(uint16_t),
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_monolith_buf) {
            s_monolith_dsc.data = (const uint8_t*)s_monolith_buf;
            ESP_LOGI(TAG, "Allocated 1-bit TARS Monolith PSRAM canvas (%u KB)",
                     (unsigned)((TARS_MONO_W * TARS_MONO_H * 2) / 1024));
        }
    }

    display_manager_set_timeout(120000);
    update_wake_pill_ui(nullptr, nullptr, 0, false, true);

    // Pure Pitch-Black AMOLED root background
    _bg_obj = lv_obj_create(lv_scr_act());
    lv_obj_set_size(_bg_obj, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(_bg_obj, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(_bg_obj, 0, 0);
    lv_obj_set_style_radius(_bg_obj, 0, 0);
    lv_obj_set_style_pad_all(_bg_obj, 0, 0);
    lv_obj_clear_flag(_bg_obj, LV_OBJ_FLAG_SCROLLABLE);

    // 1. Top Header: Back (<) | TARS + Style/Humor/Bat | Tone Switcher Pill
    _btn_back = lv_obj_create(_bg_obj);
    lv_obj_add_flag(_btn_back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(_btn_back, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(_btn_back, 0, 0);
    lv_obj_set_size(_btn_back, 40, 40);
    lv_obj_align(_btn_back, LV_ALIGN_TOP_LEFT, 18, 12);
    lv_obj_set_style_radius(_btn_back, 10, 0);
    lv_obj_set_style_bg_color(_btn_back, lv_color_hex(0x11141B), 0);
    lv_obj_set_style_border_color(_btn_back, lv_color_hex(0x334155), 0);
    lv_obj_set_style_border_width(_btn_back, 1, 0);
    lv_obj_set_style_shadow_width(_btn_back, 0, 0);
    lv_obj_add_event_cb(_btn_back, on_tars_back_clicked, LV_EVENT_CLICKED, this);

    lv_obj_t* lbl_back = lv_label_create(_btn_back);
    lv_obj_set_style_text_font(lbl_back, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(lbl_back, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(lbl_back, LV_SYMBOL_LEFT);
    lv_obj_center(lbl_back);

    _title_label = lv_label_create(_bg_obj);
    lv_obj_set_style_text_font(_title_label, &lv_font_montserrat_20, 0);
    lv_label_set_text(_title_label, "TARS");
    lv_obj_align(_title_label, LV_ALIGN_TOP_LEFT, 68, 12);

    _params_label = lv_label_create(_bg_obj);
    lv_obj_set_style_text_font(_params_label, &lv_font_montserrat_12, 0);
    lv_label_set_text_fmt(_params_label, "%s | MIZAH %%%d | PIL %%%d",
                          kTarsStyleNames[_monolith_style], _humor_pct, _cached_bat_pct);
    lv_obj_align(_params_label, LV_ALIGN_TOP_LEFT, 68, 36);

    // Top-right Tone Switcher Button (Titanyum Beyaz -> Buz Celik -> Amber -> Yesil)
    _btn_tone = lv_obj_create(_bg_obj);
    lv_obj_add_flag(_btn_tone, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(_btn_tone, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(_btn_tone, 0, 0);
    lv_obj_set_size(_btn_tone, 84, 34);
    lv_obj_align(_btn_tone, LV_ALIGN_TOP_RIGHT, -18, 15);
    lv_obj_set_style_radius(_btn_tone, 8, 0);
    lv_obj_set_style_bg_color(_btn_tone, lv_color_hex(0x0D1017), 0);
    lv_obj_set_style_border_width(_btn_tone, 2, 0);
    lv_obj_set_style_shadow_width(_btn_tone, 0, 0);
    lv_obj_add_event_cb(_btn_tone, [](lv_event_t* e) {
        AppTars* self = (AppTars*)lv_event_get_user_data(e);
        if (self) self->cycle_tone();
    }, LV_EVENT_CLICKED, this);

    _btn_tone_label = lv_label_create(_btn_tone);
    lv_obj_set_style_text_font(_btn_tone_label, &lv_font_montserrat_12, 0);
    lv_label_set_text(_btn_tone_label, kTarsPalettes[_monolith_tone].name);
    lv_obj_center(_btn_tone_label);

    // 2. Live 1-Bit Pixel TARS Monolith Viewport (352x192, clickable -> cycles Saf / NASA / Asimetrik!)
    if (s_monolith_buf) {
        _monolith_img = lv_image_create(_bg_obj);
        lv_image_set_src(_monolith_img, &s_monolith_dsc);
        lv_obj_set_size(_monolith_img, TARS_MONO_W + 4, TARS_MONO_H + 4);
        lv_obj_align(_monolith_img, LV_ALIGN_TOP_MID, 0, 58);
        lv_obj_set_style_bg_color(_monolith_img, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(_monolith_img, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(_monolith_img, 2, 0);
        lv_obj_set_style_radius(_monolith_img, 6, 0);
        lv_obj_set_style_pad_all(_monolith_img, 0, 0);
        lv_obj_add_flag(_monolith_img, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(_monolith_img, [](lv_event_t* e) {
            AppTars* self = (AppTars*)lv_event_get_user_data(e);
            if (self) self->cycle_style();
        }, LV_EVENT_CLICKED, this);
    }

    // 3. Compact Status & Action Readout Strip
    _status_label = lv_label_create(_bg_obj);
    lv_obj_set_style_text_font(_status_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_status_label, lv_color_hex(0x00E676), 0);
    lv_label_set_text(_status_label, "HAZIR - ELLER SERBEST");
    lv_obj_align(_status_label, LV_ALIGN_TOP_MID, 0, 258);

    _action_badge = lv_label_create(_bg_obj);
    lv_obj_set_style_text_font(_action_badge, &lv_font_montserrat_12, 0);
    lv_label_set_text(_action_badge, "DOKUN: STIL DEGISTIR | UST: TON");
    lv_obj_align(_action_badge, LV_ALIGN_TOP_MID, 0, 277);

    // 4. 1-Bit Terminal Dialogue Card (cleanly framed below Monolith, scrollable)
    _dialog_card = lv_obj_create(_bg_obj);
    lv_obj_set_size(_dialog_card, 374, 134);
    lv_obj_align(_dialog_card, LV_ALIGN_TOP_MID, 0, 297);
    lv_obj_set_style_bg_color(_dialog_card, lv_color_hex(0x05070B), 0);
    lv_obj_set_style_border_width(_dialog_card, 2, 0);
    lv_obj_set_style_radius(_dialog_card, 8, 0);
    lv_obj_set_style_pad_all(_dialog_card, 11, 0);
    lv_obj_set_style_pad_row(_dialog_card, 8, 0);
    lv_obj_set_flex_flow(_dialog_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(_dialog_card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scrollbar_mode(_dialog_card, LV_SCROLLBAR_MODE_AUTO);

    _user_label = lv_label_create(_dialog_card);
    lv_obj_set_width(_user_label, LV_PCT(100));
    lv_label_set_long_mode(_user_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(_user_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_user_label, lv_color_hex(0x94A3B8), 0);
    lv_label_set_text(_user_label, "> TARS hazir. Dokun veya konus...");

    _tars_label = lv_label_create(_dialog_card);
    lv_obj_set_width(_tars_label, LV_PCT(100));
    lv_label_set_long_mode(_tars_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(_tars_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(_tars_label, lv_color_hex(0xF8FAFC), 0);
    lv_label_set_text(_tars_label, "TARS: \"Mizah %75. Seni dinliyorum Eymen.\"");

    // 5. Bottom Control Row: [Konus (174)] [TURBO (96)] [M:%75 (88)]
    _btn_test = lv_btn_create(_bg_obj);
    lv_obj_set_size(_btn_test, 174, 46);
    lv_obj_align(_btn_test, LV_ALIGN_BOTTOM_LEFT, 18, -16);
    lv_obj_set_style_radius(_btn_test, 10, 0);
    lv_obj_set_style_bg_color(_btn_test, lv_color_hex(0x1E40AF), 0);
    lv_obj_set_style_border_color(_btn_test, lv_color_hex(0x3B82F6), 0);
    lv_obj_set_style_border_width(_btn_test, 1, 0);
    lv_obj_set_style_shadow_width(_btn_test, 0, 0);
    lv_obj_add_event_cb(_btn_test, [](lv_event_t* e) {
        s_tars_manual_toggle = true;
        bsp_vibrate_pulse(35);
    }, LV_EVENT_CLICKED, this);

    _btn_test_label = lv_label_create(_btn_test);
    lv_obj_set_style_text_font(_btn_test_label, &lv_font_montserrat_14, 0);
    lv_label_set_text(_btn_test_label, LV_SYMBOL_AUDIO " Konus");
    lv_obj_center(_btn_test_label);

    _btn_turbo = lv_btn_create(_bg_obj);
    lv_obj_set_size(_btn_turbo, 96, 46);
    lv_obj_align(_btn_turbo, LV_ALIGN_BOTTOM_LEFT, 200, -16);
    lv_obj_set_style_radius(_btn_turbo, 10, 0);
    lv_obj_set_style_border_width(_btn_turbo, 1, 0);
    lv_obj_set_style_shadow_width(_btn_turbo, 0, 0);
    lv_obj_add_event_cb(_btn_turbo, [](lv_event_t* e) {
        AppTars* self = (AppTars*)lv_event_get_user_data(e);
        if (self) {
            self->set_turbo_mode(!self->is_turbo_mode());
            self->apply_theme_chrome();
            self->render_monolith_frame();
            bsp_vibrate_pulse(45);
        }
    }, LV_EVENT_CLICKED, this);

    _btn_turbo_label = lv_label_create(_btn_turbo);
    lv_obj_set_style_text_font(_btn_turbo_label, &lv_font_montserrat_14, 0);
    lv_label_set_text(_btn_turbo_label, "TURBO");
    lv_obj_center(_btn_turbo_label);

    _btn_humor = lv_btn_create(_bg_obj);
    lv_obj_set_size(_btn_humor, 88, 46);
    lv_obj_align(_btn_humor, LV_ALIGN_BOTTOM_LEFT, 304, -16);
    lv_obj_set_style_radius(_btn_humor, 10, 0);
    lv_obj_set_style_bg_color(_btn_humor, lv_color_hex(0x1C222D), 0);
    lv_obj_set_style_border_color(_btn_humor, lv_color_hex(0x334155), 0);
    lv_obj_set_style_border_width(_btn_humor, 1, 0);
    lv_obj_set_style_shadow_width(_btn_humor, 0, 0);
    lv_obj_add_event_cb(_btn_humor, [](lv_event_t* e) {
        AppTars* self = (AppTars*)lv_event_get_user_data(e);
        if (self) {
            int next = (self->get_humor() == 75) ? 100 : (self->get_humor() == 100) ? 40 : 75;
            self->set_humor(next);
            bsp_vibrate_pulse(35);
        }
    }, LV_EVENT_CLICKED, this);

    _btn_humor_label = lv_label_create(_btn_humor);
    lv_obj_set_style_text_font(_btn_humor_label, &lv_font_montserrat_14, 0);
    lv_label_set_text_fmt(_btn_humor_label, "M:%%%d", _humor_pct);
    lv_obj_center(_btn_humor_label);

    apply_theme_chrome();
    _idle_skip_div = 0;
    render_monolith_frame();
    _anim_timer = lv_timer_create(on_tars_anim_timer, 80, this); // Smart 12.5 FPS active / ~4 FPS idle

    s_tars_app_active = true;
    ensure_tars_worker();
    if (s_tars_task) xTaskNotifyGive(s_tars_task);
    return true;
}

bool AppTars::back() {
    return notifyCoreClosed();
}

bool AppTars::close() {
    s_tars_app_active = false;
    s_tars_abort_audio = true;
    display_manager_set_timeout(5000);

    if (_anim_timer) {
        lv_timer_delete(_anim_timer);
        _anim_timer = nullptr;
    }

    if (_bg_obj) {
        lv_obj_delete_async(_bg_obj);
        _bg_obj = nullptr;
        _btn_back = nullptr;
        _title_label = nullptr;
        _params_label = nullptr;
        _btn_tone = nullptr;
        _btn_tone_label = nullptr;
        _monolith_img = nullptr;
        _status_label = nullptr;
        _dialog_card = nullptr;
        _user_label = nullptr;
        _tars_label = nullptr;
        _action_badge = nullptr;
        _btn_turbo = nullptr;
        _btn_turbo_label = nullptr;
        _btn_humor = nullptr;
        _btn_humor_label = nullptr;
        _btn_test = nullptr;
        _btn_test_label = nullptr;
    }
    return true;
}
