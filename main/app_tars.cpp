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

// Floating wake/reply overlay card (positioned comfortably below top rounded bezel & status bar)
static lv_obj_t* s_wake_pill = nullptr;
static lv_obj_t* s_wake_pill_hdr = nullptr;
static lv_obj_t* s_wake_pill_lbl = nullptr;

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
        bool ok = (strstr(low, "isik") || strstr(low, "lamba") || strstr(low, "salon") ||
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
                   strstr(low, "ekran") || strstr(low, "monitor") || strstr(low, "kapat") ||
                   strstr(low, "uyut") || strstr(low, "uyku") || strstr(low, "kilit") ||
                   strstr(low, "uyandir") || strstr(low, "iptal") || strstr(low, "ac"));
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

// Execute a single action command (called sequentially by multi-action chain dispatcher!)
static bool tars_execute_single_action(const char* raw_item, const char* user_transcript_ascii, char* out_badge, size_t max_badge) {
    if (!raw_item) return false;
    char cmd[220];
    cmd[0] = '\0';
    const char* s = raw_item;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r' || s[len - 1] == '\n')) len--;
    if (len == 0) return false;
    if (len >= sizeof(cmd)) len = sizeof(cmd) - 1;
    memcpy(cmd, s, len);
    cmd[len] = '\0';

    const char* p = cmd;
    if (!tars_is_action_authorized(p, user_transcript_ascii)) {
        return false;
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
            strncpy(s_live_pc_telemetry, "PC ulasilamadi (UZ_PC_Agent kapali olabilir)", sizeof(s_live_pc_telemetry) - 1);
            strncpy(out_badge, "PC DURUM ALINAMADI", max_badge - 1);
        }
    } else if (strcmp(p, "PC_MODE_BREAK") == 0) {
        pc_control_send("PC_MODE_BREAK");
        pc_control_esphome("/switch/salon_isigi/turn_on");
        strncpy(out_badge, "MOD: MUTFAK MOLASI", max_badge - 1);
    } else if (strcmp(p, "PC_MODE_MOVIE") == 0) {
        pc_control_send("PC_MODE_MOVIE");
        pc_control_esphome("/switch/salon_isigi/turn_off");
        strncpy(out_badge, "MOD: SINEMA", max_badge - 1);
    } else if (strcmp(p, "PC_MODE_STUDY") == 0) {
        pc_control_send("PC_MODE_STUDY");
        strncpy(out_badge, "MOD: DERS / ODAK", max_badge - 1);
    } else if (strcmp(p, "PC_MODE_GAME") == 0) {
        pc_control_send("PC_MODE_GAME");
        strncpy(out_badge, "MOD: OYUN", max_badge - 1);
    } else if (strncmp(p, "PC_", 3) == 0) {
        bool ok = pc_control_send(p);
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
        else snprintf(out_badge, max_badge - 1, "PC (%s)", ok ? "OK" : "ERISILEMEDI");
    } else if (strncmp(p, "TV_", 3) == 0) {
        bool ok = tv_control_send(p);
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
        else snprintf(out_badge, max_badge - 1, "TV (%s)", ok ? "OK" : "ERISILEMEDI");
    } else if (strncmp(p, "HUMOR_SET:", 10) == 0) {
        int new_h = atoi(p + 10);
        if (new_h >= 0 && new_h <= 100 && s_tars_instance) {
            s_tars_instance->set_humor(new_h);
        }
        snprintf(out_badge, max_badge - 1, "MIZAH %%%d YAPILDI", new_h);
    } else {
        return false;
    }
    out_badge[max_badge - 1] = '\0';
    return true;
}

// Execute 1 or MULTIPLE chained actions separated by ';' (e.g. [ACTION: SALON_OFF; PC_VOL_SET:60; PC_GOOGLE_SEARCH:Starship])
static bool tars_fire_action_tag(const char* tag_inner, const char* user_transcript_ascii, char* out_action_badge, size_t max_badge) {
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
        if (tars_execute_single_action(cur, user_transcript_ascii, one_badge, sizeof(one_badge))) {
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

static void on_wake_pill_clicked(lv_event_t* e) {
    s_wake_pill_dismissed = true;
    if (s_wake_pill) {
        lv_obj_add_flag(s_wake_pill, LV_OBJ_FLAG_HIDDEN);
    }
}

// Show / update floating TARS Card positioned cleanly BELOW the curved top bezel & status bar (y = 88)
static void update_wake_pill_ui(const char* header_text, const char* body_text, uint32_t accent_hex, bool visible) {
    if (!bsp_display_lock(150)) return;
    if (!visible) {
        if (s_wake_pill) {
            lv_obj_del(s_wake_pill);
            s_wake_pill = nullptr;
            s_wake_pill_hdr = nullptr;
            s_wake_pill_lbl = nullptr;
        }
        bsp_display_unlock();
        return;
    }

    if (!s_wake_pill) {
        s_wake_pill = lv_obj_create(lv_layer_top());
        lv_obj_set_width(s_wake_pill, 364);
        lv_obj_set_height(s_wake_pill, LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(s_wake_pill, 78, 0);
        lv_obj_set_style_max_height(s_wake_pill, 280, 0);
        lv_obj_align(s_wake_pill, LV_ALIGN_TOP_MID, 0, 88);
        lv_obj_set_style_radius(s_wake_pill, 22, 0);
        lv_obj_set_style_bg_color(s_wake_pill, lv_color_hex(0x0B0F17), 0);
        lv_obj_set_style_bg_opa(s_wake_pill, LV_OPA_90, 0);
        lv_obj_set_style_border_width(s_wake_pill, 2, 0);
        lv_obj_set_style_shadow_width(s_wake_pill, 0, 0);
        lv_obj_set_style_pad_hor(s_wake_pill, 16, 0);
        lv_obj_set_style_pad_ver(s_wake_pill, 12, 0);
        lv_obj_set_style_pad_row(s_wake_pill, 6, 0);
        lv_obj_set_flex_flow(s_wake_pill, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(s_wake_pill, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_scrollbar_mode(s_wake_pill, LV_SCROLLBAR_MODE_AUTO);
        lv_obj_add_flag(s_wake_pill, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_wake_pill, on_wake_pill_clicked, LV_EVENT_CLICKED, nullptr);

        s_wake_pill_hdr = lv_label_create(s_wake_pill);
        lv_obj_set_width(s_wake_pill_hdr, LV_PCT(100));
        lv_obj_set_style_text_font(s_wake_pill_hdr, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_align(s_wake_pill_hdr, LV_TEXT_ALIGN_LEFT, 0);

        s_wake_pill_lbl = lv_label_create(s_wake_pill);
        lv_obj_set_width(s_wake_pill_lbl, LV_PCT(100));
        lv_label_set_long_mode(s_wake_pill_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(s_wake_pill_lbl, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(s_wake_pill_lbl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_align(s_wake_pill_lbl, LV_TEXT_ALIGN_LEFT, 0);
    }

    lv_obj_clear_flag(s_wake_pill, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_border_color(s_wake_pill, lv_color_hex(accent_hex), 0);
    if (s_wake_pill_hdr) {
        lv_obj_set_style_text_color(s_wake_pill_hdr, lv_color_hex(accent_hex), 0);
        lv_label_set_text(s_wake_pill_hdr, header_text ? header_text : "TARS");
    }
    if (s_wake_pill_lbl && body_text) {
        lv_label_set_text(s_wake_pill_lbl, body_text);
    }
    lv_obj_scroll_to_y(s_wake_pill, 0, LV_ANIM_OFF);
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

// Parse & Execute [ACTION: ...] tags cleanly AFTER Groq TLS finishes (zero nested sockets, staggered power draw!)
static void tars_finalize_reply(const char* raw_reply, bool action_already_fired,
                                char* out_clean_reply, size_t max_reply,
                                char* out_action_badge, size_t max_badge,
                                const char* user_transcript_ascii = nullptr) {
    out_clean_reply[0] = '\0';
    if (!action_already_fired) {
        strncpy(out_action_badge, "EYLEM: YOK (SOHBET)", max_badge - 1);
        out_action_badge[max_badge - 1] = '\0';
    }
    if (!raw_reply) return;

    char act_cmd[256] = {0};
    char* clean_utf8 = (char*)heap_caps_calloc(1, TARS_TEXT_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!clean_utf8) clean_utf8 = (char*)calloc(1, TARS_TEXT_CAP);
    if (!clean_utf8) return;

    tars_extract_and_strip_actions(raw_reply, act_cmd, sizeof(act_cmd), clean_utf8, TARS_TEXT_CAP);

    bool action_triggered = action_already_fired;
    if (!action_already_fired && act_cmd[0] != '\0') {
        action_triggered = tars_fire_action_tag(act_cmd, user_transcript_ascii, out_action_badge, max_badge);
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
    if (s_live_pc_telemetry[0] != '\0') {
        size_t cur_l = strlen(out_clean_reply);
        if (cur_l + strlen(s_live_pc_telemetry) + 12 < max_reply) {
            snprintf(out_clean_reply + cur_l, max_reply - cur_l, "\n[PC] %s", s_live_pc_telemetry);
        }
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

// Persistent HTTP/SSE Context for Groq Keep-Alive Client
enum GroqHttpMode {
    GROQ_MODE_DISCARD = 0,
    GROQ_MODE_JSON = 1,
    GROQ_MODE_SSE_STREAM = 2,
};

struct GroqHttpCtx {
    GroqHttpMode mode;
    char* json_buf;
    size_t json_len;
    size_t json_cap;

    // SSE Streaming state
    char* sse_line_buf;
    size_t sse_line_len;
    size_t sse_line_cap;

    char* reply_utf8;
    size_t reply_len;
    size_t reply_cap;
    char* ui_ascii_tmp;

    bool is_wake_window;
    bool action_checked;
    bool action_fired;
    size_t spoken_offset;
    char action_badge[64];

    int64_t chat_start_us;
    int64_t first_token_us;
    int64_t action_us;
    int64_t last_ui_push_us;
    size_t last_ui_len;
};

static GroqHttpCtx s_http_ctx = {};

static void tars_process_sse_line(GroqHttpCtx* ctx, const char* line) {
    if (strncmp(line, "data: ", 6) != 0) return;
    const char* payload = line + 6;
    if (strcmp(payload, "[DONE]") == 0) return;

    cJSON* obj = cJSON_Parse(payload);
    if (!obj) return;

    cJSON* choices = cJSON_GetObjectItem(obj, "choices");
    if (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
        cJSON* c0 = cJSON_GetArrayItem(choices, 0);
        cJSON* delta = c0 ? cJSON_GetObjectItem(c0, "delta") : nullptr;
        cJSON* cont = delta ? cJSON_GetObjectItem(delta, "content") : nullptr;
        if (cont && cJSON_IsString(cont) && cont->valuestring[0] != '\0') {
            int64_t now = esp_timer_get_time();
            if (ctx->first_token_us == 0) {
                ctx->first_token_us = now;
            }
            size_t dlen = strlen(cont->valuestring);
            if (ctx->reply_len + dlen >= ctx->reply_cap) {
                dlen = (ctx->reply_cap - 1) - ctx->reply_len;
            }
            if (dlen > 0) {
                memcpy(ctx->reply_utf8 + ctx->reply_len, cont->valuestring, dlen);
                ctx->reply_len += dlen;
                ctx->reply_utf8[ctx->reply_len] = '\0';
            }

            // Hide leading [ACTION: ...] tag during live SSE preview; execute actions SAFELY AFTER HTTP closes!
            // (Eliminates nested 2nd TLS/TCP session & heap/I2C contention inside Groq's SSE callback!)
            if (!ctx->action_checked) {
                const char* p = ctx->reply_utf8;
                while (*p == ' ' || *p == '\n' || *p == '\r') p++;
                if (*p == '[') {
                    const char* close_br = strchr(p, ']');
                    if (close_br) {
                        if (strncmp(p, "[ACTION:", 8) == 0 && (close_br - p) < 256) {
                            const char* after = close_br + 1;
                            while (*after == ' ' || *after == '\n' || *after == '\r') after++;
                            ctx->spoken_offset = (size_t)(after - ctx->reply_utf8);
                        } else {
                            ctx->spoken_offset = (size_t)(p - ctx->reply_utf8);
                        }
                        ctx->action_checked = true;
                    } else if ((size_t)(ctx->reply_utf8 + ctx->reply_len - p) > 250) {
                        ctx->spoken_offset = (size_t)(p - ctx->reply_utf8);
                        ctx->action_checked = true;
                    }
                } else if (*p != '\0') {
                    ctx->spoken_offset = (size_t)(p - ctx->reply_utf8);
                    ctx->action_checked = true;
                }
            }

            // Stream live words onto watch display every ~120ms (strip any trailing [ACTION: ...] tag cleanly)
            if (ctx->action_checked && ctx->ui_ascii_tmp) {
                const char* spoken = ctx->reply_utf8 + ctx->spoken_offset;
                while (*spoken == ' ' || *spoken == '\n' || *spoken == '\r') spoken++;
                const char* tail_tag = strstr(spoken, "[ACTION:");
                size_t slen = tail_tag ? (size_t)(tail_tag - spoken) : strlen(spoken);
                while (slen > 0 && (spoken[slen - 1] == ' ' || spoken[slen - 1] == '\n' || spoken[slen - 1] == '\r')) slen--;
                if (slen > 0 && ((now - ctx->last_ui_push_us) > 120000LL || (slen > ctx->last_ui_len && (slen - ctx->last_ui_len) >= 28))) {
                    ctx->last_ui_push_us = now;
                    ctx->last_ui_len = slen;
                    char preview_utf8[320];
                    size_t plen = (slen < sizeof(preview_utf8) - 1) ? slen : (sizeof(preview_utf8) - 1);
                    memcpy(preview_utf8, spoken, plen);
                    preview_utf8[plen] = '\0';
                    tars_utf8_to_ascii(ctx->ui_ascii_tmp, preview_utf8, TARS_TEXT_CAP);
                    display_manager_reset_timer();
                    if (ctx->is_wake_window) {
                        update_wake_pill_ui("TARS // CANLI YANIT", ctx->ui_ascii_tmp, 0x00E676, true);
                    } else if (s_tars_instance) {
                        s_tars_instance->update_ui_state("TARS YANITLIYOR (CANLI)...", 0x00E676, 0, nullptr, ctx->ui_ascii_tmp, nullptr);
                    }
                }
            }
        }
    }
    cJSON_Delete(obj);
}

static esp_err_t groq_http_event_handler(esp_http_client_event_t *evt) {
    GroqHttpCtx* ctx = (GroqHttpCtx*)evt->user_data;
    if (!ctx || evt->event_id != HTTP_EVENT_ON_DATA || evt->data_len <= 0) {
        return ESP_OK;
    }

    if (ctx->mode == GROQ_MODE_JSON && ctx->json_buf) {
        size_t copy_len = (size_t)evt->data_len;
        if (ctx->json_len + copy_len >= ctx->json_cap) {
            copy_len = (ctx->json_cap - 1) - ctx->json_len;
        }
        if (copy_len > 0) {
            memcpy(ctx->json_buf + ctx->json_len, evt->data, copy_len);
            ctx->json_len += copy_len;
            ctx->json_buf[ctx->json_len] = '\0';
        }
    } else if (ctx->mode == GROQ_MODE_SSE_STREAM && ctx->sse_line_buf) {
        const char* src = (const char*)evt->data;
        for (int i = 0; i < evt->data_len; i++) {
            char ch = src[i];
            if (ch == '\n') {
                if (ctx->sse_line_len > 0 && ctx->sse_line_buf[ctx->sse_line_len - 1] == '\r') {
                    ctx->sse_line_len--;
                }
                ctx->sse_line_buf[ctx->sse_line_len] = '\0';
                if (ctx->sse_line_len > 0) {
                    tars_process_sse_line(ctx, ctx->sse_line_buf);
                }
                ctx->sse_line_len = 0;
            } else if (ctx->sse_line_len + 1 < ctx->sse_line_cap) {
                ctx->sse_line_buf[ctx->sse_line_len++] = ch;
            }
        }
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
    // Recreate if idle > 45s so Cloudflare/Groq keep-alive never stalls on a stale half-closed socket
    int64_t now = esp_timer_get_time();
    if (s_groq_client && s_groq_warm && (now - s_groq_last_use_us) > 45000000LL) {
        tars_reset_groq_client();
    }
    if (!s_groq_client) {
        esp_http_client_config_t config = {};
        config.url = "https://api.groq.com/openai/v1/models";
        config.event_handler = groq_http_event_handler;
        config.user_data = &s_http_ctx;
        config.crt_bundle_attach = esp_crt_bundle_attach;
        config.timeout_ms = 12000;
        config.keep_alive_enable = true;
        config.keep_alive_idle = 40;
        config.keep_alive_interval = 10;
        config.keep_alive_count = 3;
        config.buffer_size = 2048;
        config.buffer_size_tx = 4096; // Larger TX buffer = much faster TLS audio upload on ESP32-S3!
        s_groq_client = esp_http_client_init(&config);
        s_groq_warm = false;
    }
    return s_groq_client;
}

// Background TLS Pre-Warm: performs TLS 1.3 handshake BEFORE user finishes speaking -> 0ms TLS on voice command!
static void tars_prewarm_groq_tls(void) {
    if (s_groq_warm || !wifi_manager_is_connected() || wifi_manager_is_ap_active()) return;
    tars_load_api_key();
    if (strlen(s_groq_api_key) < 15) return;

    esp_http_client_handle_t client = tars_get_groq_client();
    if (!client) return;

    int64_t t0 = esp_timer_get_time();
    char auth_hdr[200];
    snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", s_groq_api_key);

    s_http_ctx.mode = GROQ_MODE_DISCARD;
    esp_http_client_set_url(client, "https://api.groq.com/openai/v1/models");
    esp_http_client_set_method(client, HTTP_METHOD_GET);
    esp_http_client_set_header(client, "Authorization", auth_hdr);
    esp_http_client_delete_header(client, "Content-Type");
    esp_http_client_set_post_field(client, nullptr, 0);

    wifi_manager_set_turbo(true);
    esp_err_t err = esp_http_client_perform(client);
    wifi_manager_set_turbo(false);

    if (err == ESP_OK && esp_http_client_get_status_code(client) == 200) {
        s_groq_warm = true;
        s_groq_last_use_us = esp_timer_get_time();
        ESP_LOGI(TAG, "Groq TLS 1.3 Pre-Warmed in %lld ms! Next voice turn TLS = 0ms.",
                 (long long)((s_groq_last_use_us - t0) / 1000));
    } else {
        ESP_LOGW(TAG, "Groq TLS pre-warm note: err=%s status=%d", esp_err_to_name(err),
                 esp_http_client_get_status_code(client));
        tars_reset_groq_client();
    }
}

enum TarsPipelineResult {
    TARS_RES_OK = 0,
    TARS_RES_NO_WIFI = 1,
    TARS_RES_SILENCE_HALLUCINATION = 2,
    TARS_RES_CLOUD_ERR = 3,
};

struct TarsTimingStats {
    int stt_ms;
    int first_tok_ms;
    int action_ms;
    int total_ms;
    bool was_warm_tls;
    bool action_fired_live;
    char live_action_badge[64];
};

// 3x Turbo Groq LPU Pipeline: Persistent Warm TLS + 2x Smaller u-Law WAV + Wi-Fi Turbo Burst + Live SSE Token Streaming!
static TarsPipelineResult tars_run_groq_turbo_pipeline(const uint8_t* wav_data, size_t wav_len,
                                                       bool is_wake_window,
                                                       char* out_transcript_ascii, size_t max_trans,
                                                       char* out_reply_utf8, size_t max_reply,
                                                       TarsTimingStats* out_stats) {
    memset(out_stats, 0, sizeof(*out_stats));
    s_live_pc_telemetry[0] = '\0';
    tars_load_api_key();
    if (strlen(s_groq_api_key) < 15) return TARS_RES_CLOUD_ERR;
    if (wifi_manager_is_ap_active()) return TARS_RES_NO_WIFI;

    if (!wifi_manager_is_connected()) {
        if (is_wake_window) {
            update_wake_pill_ui("TARS // WI-FI", "Ev Wi-Fi baglaniyor...", 0xFFA000, true);
        } else if (s_tars_instance) {
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
        update_wake_pill_ui("TARS // TURBO STT", "Turkce konusma cozuluyor...", 0x00E5FF, true);
    } else if (s_tars_instance) {
        s_tars_instance->update_ui_state("TURBO STT (u-Law + Whisper v3)...", 0x00E5FF, 0, nullptr, nullptr, nullptr);
    }

    char* resp_mem = (char*)heap_caps_malloc(8192, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char* sse_line = (char*)heap_caps_malloc(2048, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char* ui_ascii = (char*)heap_caps_malloc(TARS_TEXT_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!resp_mem || !sse_line || !ui_ascii) {
        if (resp_mem) free(resp_mem);
        if (sse_line) free(sse_line);
        if (ui_ascii) free(ui_ascii);
        return TARS_RES_CLOUD_ERR;
    }

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
        free(sse_line);
        free(ui_ascii);
        return TARS_RES_CLOUD_ERR;
    }
    memcpy(body_buf, part_head, head_len);
    memcpy(body_buf + head_len, wav_data, wav_len);
    memcpy(body_buf + head_len + wav_len, part_tail, tail_len);

    char ctype_hdr[96];
    snprintf(ctype_hdr, sizeof(ctype_hdr), "multipart/form-data; boundary=%s", boundary);

    // Activate Wi-Fi Turbo Burst (WIFI_PS_NONE) strictly for the ~1s Groq upload + SSE stream!
    wifi_manager_mark_activity();
    wifi_manager_set_turbo(true);

    out_stats->was_warm_tls = s_groq_warm;
    esp_err_t err = ESP_FAIL;
    int status_code = 0;

    // Step 1: Whisper-Large-v3 STT on Persistent Warm TLS Client (with automatic 1-retry if keep-alive socket went stale)
    for (int attempt = 0; attempt < 2; attempt++) {
        esp_http_client_handle_t client = tars_get_groq_client();
        if (!client) break;

        s_http_ctx.mode = GROQ_MODE_JSON;
        s_http_ctx.json_buf = resp_mem;
        s_http_ctx.json_len = 0;
        s_http_ctx.json_cap = 8192;
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
        ESP_LOGW(TAG, "Groq STT attempt %d failed (err=%s, http=%d) -> resetting TLS socket",
                 attempt + 1, esp_err_to_name(err), status_code);
        tars_reset_groq_client();
        out_stats->was_warm_tls = false;
    }

    free(body_buf);
    int64_t t_stt_done = esp_timer_get_time();
    out_stats->stt_ms = (int)((t_stt_done - t_pipe_start) / 1000);
    display_manager_reset_timer();

    if (err != ESP_OK || status_code != 200) {
        wifi_manager_set_turbo(false);
        free(resp_mem);
        free(sse_line);
        free(ui_ascii);
        return TARS_RES_CLOUD_ERR;
    }

    cJSON* stt_json = cJSON_Parse(resp_mem);
    if (!stt_json) {
        wifi_manager_set_turbo(false);
        free(resp_mem);
        free(sse_line);
        free(ui_ascii);
        return TARS_RES_CLOUD_ERR;
    }
    cJSON* text_item = cJSON_GetObjectItem(stt_json, "text");
    if (!text_item || !cJSON_IsString(text_item) || strlen(text_item->valuestring) < 2) {
        cJSON_Delete(stt_json);
        wifi_manager_set_turbo(false);
        free(resp_mem);
        free(sse_line);
        free(ui_ascii);
        return TARS_RES_SILENCE_HALLUCINATION;
    }

    char* raw_transcript = (char*)heap_caps_malloc(TARS_TEXT_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!raw_transcript) {
        cJSON_Delete(stt_json);
        wifi_manager_set_turbo(false);
        free(resp_mem);
        free(sse_line);
        free(ui_ascii);
        return TARS_RES_CLOUD_ERR;
    }
    const char* t_src = text_item->valuestring;
    while (*t_src == ' ' || *t_src == '\n' || *t_src == '\r') t_src++;
    strncpy(raw_transcript, t_src, TARS_TEXT_CAP - 1);
    raw_transcript[TARS_TEXT_CAP - 1] = '\0';
    tars_utf8_to_ascii(out_transcript_ascii, raw_transcript, max_trans);
    cJSON_Delete(stt_json);

    ESP_LOGI(TAG, "STT (%dms, %uKB u-law, warm=%d): '%s'",
             out_stats->stt_ms, (unsigned)(wav_len / 1024), (int)out_stats->was_warm_tls, out_transcript_ascii);
    if (tars_is_whisper_hallucination(out_transcript_ascii)) {
        ESP_LOGW(TAG, "Filtered Whisper silence hallucination: '%s'", out_transcript_ascii);
        free(raw_transcript);
        wifi_manager_set_turbo(false);
        free(resp_mem);
        free(sse_line);
        free(ui_ascii);
        return TARS_RES_SILENCE_HALLUCINATION;
    }

    if (is_wake_window) {
        update_wake_pill_ui("TARS // DUSUNUYOR", out_transcript_ascii, 0xFFA000, true);
    } else if (s_tars_instance) {
        s_tars_instance->update_ui_state("TARS AKIS BASLIYOR...", 0xFFA000, 0, out_transcript_ascii, nullptr, nullptr);
    }

    // Step 2: Live SSE Token Streaming on the SAME Warm Keep-Alive TLS Client!
    int humor = s_tars_instance ? s_tars_instance->get_humor() : 75;
    int bat = bsp_battery_get_percent();
    struct tm ti = {};
    rtc_get_time(&ti);

    char* sys_prompt = (char*)heap_caps_malloc(3584, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!sys_prompt) {
        free(raw_transcript);
        wifi_manager_set_turbo(false);
        free(resp_mem);
        free(sse_line);
        free(ui_ascii);
        return TARS_RES_CLOUD_ERR;
    }
    snprintf(sys_prompt, 3584,
             "Sen Interstellar filmindeki TARS'sin. Sahibin Eymen'in akilli saatindeki zeki, sadik sesli asistansin. "
             "Mizah seviyen %%%d, durustluk %%90. Kisa, dogal 1-2 Turkce cumleyle cevap ver. "
             "Bilgi: Saat=%02d:%02d, Pil=%%%d.\n"
             "COK ONEMLI KURAL:\n"
             "1) Normal sohbet, selamlasma, soru, fikra, saat/pil sorma durumlarinda ASLA [ACTION: ...] yazma ve ASLA kendiliginden isik/TV/PC acip kapatma! (Aksam saati olsa bile kullanici acikca istemedikce isigi yakma!)\n"
             "   Ornek: Kullanici='Merhaba.' -> 'Merhaba Eymen, seni dinliyorum.'\n"
             "   Ornek: Kullanici='Bana bir fikra anlat.' -> 'Kisa bir fikra...' (ETIKETSIZ!)\n"
             "   Ornek: Kullanici='Saat kac?' -> 'Saat su an %02d:%02d, Eymen.' (ETIKETSIZ!)\n"
             "2) SADECE kullanici acikca bir cihazi/uygulamayi kontrol etmek istiyorsa cevabin EN BASINA [ACTION: KOMUT] yaz (birden fazla istek varsa ';' ile ayir) ve yanina kisa Turkce cevabini yaz:\n"
             "- Ev Isigi (SADECE isik/lamba/salon derse): SALON_ON, SALON_OFF, SALON_TOGGLE\n"
             "- TV (SADECE televizyon/TV/Tivibu/kanal derse): TV_POWER, TV_WAKE, TV_SLEEP, TV_PLAY_PAUSE, TV_PAUSE, TV_PLAY, TV_VOL_UP, TV_VOL_DOWN, TV_MUTE, TV_CH_UP, TV_CH_DOWN, TV_FWD, TV_REW, TV_NEXT, TV_PREV, TV_UP, TV_DOWN, TV_LEFT, TV_RIGHT, TV_OK, TV_BACK, TV_HOME, TV_OPEN_YOUTUBE, TV_OPEN_NETFLIX, TV_OPEN_SPOTIFY, TV_OPEN_PRIME, TV_OPEN_DISNEY, TV_YOUTUBE_PLAY:sorgu\n"
             "- PC Guc (SADECE bilgisayar/PC derse): PC_WAKE, PC_SLEEP, PC_LOCK, PC_SCREEN_OFF, PC_SHUTDOWN_SEC:30, PC_SHUTDOWN_MIN:10, PC_SHUTDOWN_CANCEL\n"
             "- PC Medya/Ses: PC_MEDIA_PLAY_PAUSE, PC_MEDIA_NEXT, PC_MEDIA_PREV, PC_VOL_UP, PC_VOL_DOWN, PC_VOL_MUTE, PC_VOL_SET:40\n"
             "- PC Web/Arama/Dikte: PC_GOOGLE_SEARCH:sorgu, PC_OPEN_URL:url, PC_YOUTUBE_PLAY:sorgu, PC_YOUTUBE_SEARCH:sorgu, PC_SPOTIFY_SEARCH:sorgu, PC_TYPE_TEXT:metin, PC_OPEN_CLIPBOARD_URL\n"
             "- PC Uygulama: PC_OPEN_APP:ad (KiCad, Creality Print, Valorant, LoL, Minecraft, OpenRocket, Betaflight, OBS, CapCut, Telegram, Antigravity, Spotify, Chrome, Discord, Steam, VS Code, Arduino, Ayarlar), PC_CLOSE_APP:ad\n"
             "- PC Video/Pencere: PC_SEEK_FWD, PC_SEEK_BACK, PC_CAPTIONS, PC_THEATER, PC_FULLSCREEN, PC_SPEED_UP, PC_SPEED_DOWN, PC_NEW_TAB, PC_REOPEN_TAB, PC_NEXT_TAB, PC_PREV_TAB, PC_CLOSE_TAB, PC_REFRESH, PC_SCROLL_DOWN, PC_SCROLL_UP, PC_ALT_TAB, PC_CLOSE_WINDOW, PC_SNAP_LEFT, PC_SNAP_RIGHT, PC_MINIMIZE_ALL, PC_BOSS_KEY\n"
             "- PC Sistem: PC_SCREENSHOT, PC_GET_STATS\n"
             "- Ayar: HUMOR_SET:50",
             humor, ti.tm_hour, ti.tm_min, bat, ti.tm_hour, ti.tm_min);

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", "openai/gpt-oss-20b");
    cJSON_AddStringToObject(root, "reasoning_effort", "low");
    cJSON_AddBoolToObject(root, "include_reasoning", false);
    cJSON_AddBoolToObject(root, "stream", true); // LIVE SSE TOKEN STREAMING!
    cJSON_AddNumberToObject(root, "max_tokens", 450);
    cJSON_AddNumberToObject(root, "temperature", 0.2);
    cJSON* msgs = cJSON_AddArrayToObject(root, "messages");

    cJSON* m_sys = cJSON_CreateObject();
    cJSON_AddStringToObject(m_sys, "role", "system");
    cJSON_AddStringToObject(m_sys, "content", sys_prompt);
    cJSON_AddItemToArray(msgs, m_sys);
    free(sys_prompt);

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

    char* chat_body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    int64_t t_chat_start = esp_timer_get_time();
    out_reply_utf8[0] = '\0';

    s_http_ctx.mode = GROQ_MODE_SSE_STREAM;
    s_http_ctx.sse_line_buf = sse_line;
    s_http_ctx.sse_line_len = 0;
    s_http_ctx.sse_line_cap = 2048;
    s_http_ctx.reply_utf8 = out_reply_utf8;
    s_http_ctx.reply_len = 0;
    s_http_ctx.reply_cap = max_reply;
    s_http_ctx.ui_ascii_tmp = ui_ascii;
    s_http_ctx.is_wake_window = is_wake_window;
    s_http_ctx.action_checked = false;
    s_http_ctx.action_fired = false;
    s_http_ctx.spoken_offset = 0;
    strncpy(s_http_ctx.action_badge, "EYLEM: YOK (SOHBET)", sizeof(s_http_ctx.action_badge) - 1);
    s_http_ctx.chat_start_us = t_chat_start;
    s_http_ctx.first_token_us = 0;
    s_http_ctx.action_us = 0;
    s_http_ctx.last_ui_push_us = 0;
    s_http_ctx.last_ui_len = 0;

    esp_http_client_handle_t client = tars_get_groq_client();
    TarsPipelineResult res = TARS_RES_CLOUD_ERR;
    if (client && chat_body) {
        esp_http_client_set_url(client, "https://api.groq.com/openai/v1/chat/completions");
        esp_http_client_set_method(client, HTTP_METHOD_POST);
        esp_http_client_set_header(client, "Authorization", auth_hdr);
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, chat_body, strlen(chat_body));

        err = esp_http_client_perform(client);
        status_code = esp_http_client_get_status_code(client);

        if (err == ESP_OK && status_code == 200 && s_http_ctx.reply_len > 0) {
            s_groq_warm = true;
            s_groq_last_use_us = esp_timer_get_time();
            // Strip [ACTION: ...] tags BEFORE saving into history so past commands NEVER echo/poison future turns!
            char* hist_clean = (char*)heap_caps_calloc(1, TARS_TEXT_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (hist_clean) {
                tars_extract_and_strip_actions(out_reply_utf8, nullptr, 0, hist_clean, TARS_TEXT_CAP);
                tars_push_history(raw_transcript, hist_clean[0] ? hist_clean : "Komut uygulandi.");
                free(hist_clean);
            }
            res = TARS_RES_OK;
        } else {
            ESP_LOGW(TAG, "Groq SSE Chat failed (err=%s, http=%d)", esp_err_to_name(err), status_code);
            tars_reset_groq_client();
        }
    }

    // Immediately restore Wi-Fi low-power modem sleep!
    wifi_manager_set_turbo(false);

    int64_t t_done = esp_timer_get_time();
    out_stats->first_tok_ms = (s_http_ctx.first_token_us > t_chat_start) ? (int)((s_http_ctx.first_token_us - t_chat_start) / 1000) : 0;
    out_stats->action_ms = (s_http_ctx.action_us > t_pipe_start) ? (int)((s_http_ctx.action_us - t_pipe_start) / 1000) : 0;
    out_stats->total_ms = (int)((t_done - t_pipe_start) / 1000);
    out_stats->action_fired_live = s_http_ctx.action_fired;
    strncpy(out_stats->live_action_badge, s_http_ctx.action_badge, sizeof(out_stats->live_action_badge) - 1);

    ESP_LOGI(TAG, "=== [TARS-3X TIMING] wav=%uKB(ulaw) warmTLS=%d | STT=%dms | 1stTok=%dms | Action=%dms | Total=%dms ===",
             (unsigned)(wav_len / 1024), (int)out_stats->was_warm_tls,
             out_stats->stt_ms, out_stats->first_tok_ms, out_stats->action_ms, out_stats->total_ms);

    if (chat_body) free(chat_body);
    free(raw_transcript);
    free(resp_mem);
    free(sse_line);
    free(ui_ascii);
    display_manager_reset_timer();
    return res;
}

extern "C" void app_tars_handle_reply_from_ble(const char* payload) {
    if (!payload) return;
    char* clean_reply = (char*)heap_caps_calloc(1, TARS_TEXT_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!clean_reply) return;
    char action_badge[64] = {0};
    tars_finalize_reply(payload, false, clean_reply, TARS_TEXT_CAP, action_badge, sizeof(action_badge));

    if (s_tars_instance && s_tars_instance->is_open()) {
        s_tars_instance->update_ui_state("ELLER SERBEST DINLIYOR...", 0x00E676, 0, nullptr, clean_reply, action_badge);
    } else {
        display_manager_turn_on();
        s_wake_pill_dismissed = false;
        update_wake_pill_ui("TARS // YANIT (Dokun: Kapat)", clean_reply, 0x00E676, true);
    }
    free(clean_reply);
}

// Unified Background Worker Task for both 4.5s Triple-Tap Wake Window & Full Hands-Free App Mode
static void tars_worker_loop(void* pv) {
    ESP_LOGI(TAG, "TARS 3X TURBO Worker started (Warm TLS + u-Law 2x + SSE Stream + 12s Long Speech)");
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
            // Release warm socket cleanly if Wi-Fi dropped or turned off by Smart 90s Auto-Off
            if (s_groq_client && !wifi_manager_is_connected()) {
                tars_reset_groq_client();
            }
            // Zero-CPU idle wait (instant 0ms wakeup via xTaskNotifyGive when Triple-Tap or TARS App triggers!)
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

        // Pre-warm Home Wi-Fi + Persistent TLS 1.3 inside TARS App BEFORE user even speaks!
        if (!is_wake_window) {
            if (!wifi_manager_is_active()) {
                wifi_manager_start_async();
            } else if (wifi_manager_is_connected() && !s_groq_warm) {
                if (s_tars_instance) {
                    s_tars_instance->update_ui_state("TURBO HAT ISITILIYOR...", 0x00E5FF, 0, nullptr, nullptr, nullptr);
                }
                tars_prewarm_groq_tls();
            }
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
            update_wake_pill_ui(LV_SYMBOL_AUDIO " TARS DINLIYOR (4.5 sn)", "Komutunu veya sorununuzu soyleyin...", 0x00E5FF, true);
        } else if (s_tars_instance) {
            s_tars_instance->update_ui_state(s_groq_warm ? "TURBO HAZIR (0ms TLS) - KONUS" : "ELLER SERBEST DINLIYOR...",
                                             0x00E676, 5, nullptr, nullptr, nullptr, LV_SYMBOL_AUDIO " Konusmayi Baslat");
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

            // DC-removed frame energy (512 samples = 32ms at 16kHz)
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
                            update_wake_pill_ui(LV_SYMBOL_AUDIO " TARS SENI DINLIYOR", "Konusmaya devam edebilirsin...", 0x00E676, true);
                        } else if (s_tars_instance) {
                            s_tars_instance->update_ui_state("KONUSMA ALGILANDI...", 0xFFA000, level_pct, "Dinleniyor...", nullptr, nullptr, LV_SYMBOL_OK " Gonder (Bitir)");
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

                // Inside TARS App, if Wi-Fi just got IP while idle and TLS isn't warm yet, break briefly to warm TLS!
                if (!is_wake_window && !s_groq_warm && wifi_manager_is_connected() && waited_ms > 400) {
                    break;
                }

                if (!is_wake_window && s_tars_instance && (waited_ms % 160 < 40)) {
                    s_tars_instance->update_ui_state(s_groq_warm ? "TURBO HAZIR (0ms TLS) - KONUS" : "ELLER SERBEST DINLIYOR...",
                                                     0x00E676, level_pct, nullptr, nullptr, nullptr);
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
                if (!is_wake_window && s_tars_instance && ((samples_written / TARS_FRAME_SAMPLES) % 5 == 0)) {
                    char rec_hdr[48];
                    snprintf(rec_hdr, sizeof(rec_hdr), "KAYDEDILIYOR (%.1f sn)...", (float)rec_ms / 1000.0f);
                    s_tars_instance->update_ui_state(rec_hdr, 0xFFA000, level_pct, nullptr, nullptr, nullptr);
                }

                // Smart Adaptive Silence Cutoff (saves ~350-500ms vs fixed timeout without chopping "TARS... [pause]"):
                // - < 1.4s speech: 34 frames (~1.08s) so wake-word pause never cuts off
                // - 1.4s..3.5s speech: 24 frames (~768ms) snappy end-of-command
                // - > 3.5s long sentence: 19 frames (~608ms) super fast end-of-sentence
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

        // Trim trailing dead silence (keep ~220ms tail)
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

        // Encode DC-removed + AGC-normalized speech into 2x smaller 16kHz u-Law WAV (<1ms CPU!)
        size_t ulaw_wav_len = tars_encode_ulaw_wav(pcm_buf, samples_written, ulaw_wav_buf);

        transcript[0] = '\0';
        raw_reply[0] = '\0';
        clean_reply[0] = '\0';
        char action_badge[64] = {0};
        TarsTimingStats stats = {};

        TarsPipelineResult res = tars_run_groq_turbo_pipeline(
            ulaw_wav_buf, ulaw_wav_len, is_wake_window,
            transcript, TARS_TEXT_CAP,
            raw_reply, TARS_TEXT_CAP,
            &stats
        );

        if (res == TARS_RES_SILENCE_HALLUCINATION) {
            if (is_wake_window) {
                s_tars_wake_window_req = false;
                update_wake_pill_ui("TARS", "Anlasilabilir bir konusma duyulmamis gibi. Tekrar deneyebilirsin.", 0x9E9E9E, true);
                for (int t = 0; t < 5 && !s_wake_pill_dismissed && display_manager_is_on(); t++) {
                    display_manager_reset_timer();
                    vTaskDelay(pdMS_TO_TICKS(450));
                }
                update_wake_pill_ui(nullptr, nullptr, 0, false);
            } else if (s_tars_instance) {
                s_tars_instance->update_ui_state("ANLASILMADI - TEKRAR KONUS", 0x9E9E9E, 0, nullptr, nullptr, "EYLEM: YOK", LV_SYMBOL_AUDIO " Konusmayi Baslat");
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
        }

        if (stats.action_fired_live) {
            strncpy(action_badge, stats.live_action_badge, sizeof(action_badge) - 1);
        }
        tars_finalize_reply(raw_reply, stats.action_fired_live, clean_reply, TARS_TEXT_CAP, action_badge, sizeof(action_badge), transcript);

        char speed_hdr[64];
        if (res == TARS_RES_OK) {
            snprintf(speed_hdr, sizeof(speed_hdr), "TARS 3X // %dms (STT:%d AI:%d)",
                     stats.total_ms, stats.stt_ms, stats.first_tok_ms);
        } else {
            snprintf(speed_hdr, sizeof(speed_hdr), "TARS // YANIT (Dokun: Kapat)");
        }

        if (is_wake_window) {
            s_tars_wake_window_req = false;
            s_wake_pill_dismissed = false;
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
                snprintf(app_status, sizeof(app_status), "HIZ: %dms (STT:%d | 1.Tok:%d)",
                         stats.total_ms, stats.stt_ms, stats.first_tok_ms);
            } else {
                snprintf(app_status, sizeof(app_status), "ELLER SERBEST DINLIYOR...");
            }
            s_tars_instance->update_ui_state(app_status, 0x00E676, 0, transcript, clean_reply, action_badge, LV_SYMBOL_AUDIO " Konusmayi Baslat");
            vTaskDelay(pdMS_TO_TICKS(400));
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

AppTars* AppTars::get_instance() {
    return s_tars_instance;
}

AppTars::AppTars()
    : esp_brookesia::systems::phone::App("TARS", &icon_voice_recorder, true),
      _bg_obj(nullptr), _btn_back(nullptr), _params_label(nullptr), _status_label(nullptr),
      _level_bar(nullptr), _dialog_card(nullptr), _user_label(nullptr), _tars_label(nullptr),
      _action_badge(nullptr), _btn_humor(nullptr), _btn_test(nullptr), _btn_test_label(nullptr),
      _humor_pct(75), _honesty_pct(90) {
    s_tars_instance = this;
}

AppTars::~AppTars() {
    if (s_tars_instance == this) {
        s_tars_instance = nullptr;
    }
}

void AppTars::set_humor(int h) {
    _humor_pct = h;
    if (_params_label && bsp_display_lock(100)) {
        if (_params_label) {
            lv_label_set_text_fmt(_params_label, "MIZAH: %%%d  |  DURUSTLUK: %%%d", _humor_pct, _honesty_pct);
        }
        bsp_display_unlock();
    }
}

void AppTars::update_ui_state(const char* status_text, uint32_t status_color, int mic_level_pct,
                              const char* user_text, const char* tars_text, const char* action_text,
                              const char* trigger_btn_text) {
    if (!_bg_obj || !bsp_display_lock(100)) return;
    if (!_bg_obj) {
        bsp_display_unlock();
        return;
    }

    if (status_text && _status_label) {
        lv_label_set_text(_status_label, status_text);
        lv_obj_set_style_text_color(_status_label, lv_color_hex(status_color), 0);
    }
    if (_level_bar && mic_level_pct >= 0) {
        lv_bar_set_value(_level_bar, mic_level_pct, LV_ANIM_OFF);
    }
    if (user_text && _user_label) {
        lv_label_set_text_fmt(_user_label, "SEN: \"%s\"", user_text);
    }
    if (tars_text && _tars_label) {
        lv_label_set_text_fmt(_tars_label, "TARS: \"%s\"", tars_text);
        if (_dialog_card) {
            lv_obj_scroll_to_y(_dialog_card, 0, LV_ANIM_OFF);
        }
    }
    if (action_text && _action_badge) {
        lv_label_set_text(_action_badge, action_text);
    }
    if (trigger_btn_text && _btn_test_label) {
        lv_label_set_text(_btn_test_label, trigger_btn_text);
    }

    bsp_display_unlock();
}

bool AppTars::run() {
    if (_bg_obj != nullptr) return true;

    display_manager_set_timeout(120000);
    update_wake_pill_ui(nullptr, nullptr, 0, false);

    _bg_obj = lv_obj_create(lv_scr_act());
    lv_obj_set_size(_bg_obj, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(_bg_obj, lv_color_hex(0x080A0F), 0);
    lv_obj_set_style_border_width(_bg_obj, 0, 0);
    lv_obj_set_style_radius(_bg_obj, 0, 0);
    lv_obj_set_style_pad_all(_bg_obj, 0, 0);
    lv_obj_clear_flag(_bg_obj, LV_OBJ_FLAG_SCROLLABLE);

    // 1. Top Header with Back Button (<) + TARS 3X Title
    _btn_back = lv_obj_create(_bg_obj);
    lv_obj_add_flag(_btn_back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(_btn_back, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(_btn_back, 0, 0);
    lv_obj_set_size(_btn_back, 42, 42);
    lv_obj_align(_btn_back, LV_ALIGN_TOP_LEFT, 16, 14);
    lv_obj_set_style_radius(_btn_back, 21, 0);
    lv_obj_set_style_bg_color(_btn_back, lv_color_hex(0x1A202C), 0);
    lv_obj_set_style_border_color(_btn_back, lv_color_hex(0x334155), 0);
    lv_obj_set_style_border_width(_btn_back, 1, 0);
    lv_obj_set_style_shadow_width(_btn_back, 0, 0);
    lv_obj_add_event_cb(_btn_back, on_tars_back_clicked, LV_EVENT_CLICKED, this);

    lv_obj_t* lbl_back = lv_label_create(_btn_back);
    lv_obj_set_style_text_font(lbl_back, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(lbl_back, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(lbl_back, LV_SYMBOL_LEFT);
    lv_obj_center(lbl_back);

    lv_obj_t* title = lv_label_create(_bg_obj);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFA000), 0);
    lv_label_set_text(title, "TARS // 3X TURBO");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 14, 14);

    _params_label = lv_label_create(_bg_obj);
    lv_obj_set_style_text_font(_params_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_params_label, lv_color_hex(0x8892B0), 0);
    lv_label_set_text_fmt(_params_label, "MIZAH: %%%d  |  DURUSTLUK: %%%d", _humor_pct, _honesty_pct);
    lv_obj_align(_params_label, LV_ALIGN_TOP_MID, 14, 42);

    // 2. Live Status & VAD Level Bar
    _status_label = lv_label_create(_bg_obj);
    lv_obj_set_style_text_font(_status_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_status_label, lv_color_hex(0x00E676), 0);
    lv_label_set_text(_status_label, "ELLER SERBEST DINLIYOR...");
    lv_obj_align(_status_label, LV_ALIGN_TOP_MID, 0, 68);

    _level_bar = lv_bar_create(_bg_obj);
    lv_obj_set_size(_level_bar, 340, 10);
    lv_obj_align(_level_bar, LV_ALIGN_TOP_MID, 0, 92);
    lv_bar_set_range(_level_bar, 0, 100);
    lv_bar_set_value(_level_bar, 8, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(_level_bar, lv_color_hex(0x1C222D), LV_PART_MAIN);
    lv_obj_set_style_bg_color(_level_bar, lv_color_hex(0x00E5FF), LV_PART_INDICATOR);
    lv_obj_set_style_radius(_level_bar, 5, 0);

    // 3. Action Status Badge
    _action_badge = lv_label_create(_bg_obj);
    lv_obj_set_style_text_font(_action_badge, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_action_badge, lv_color_hex(0xFFA000), 0);
    lv_label_set_text(_action_badge, "EYLEM: HAZIR (SICAK TLS + SSE AKIS)");
    lv_obj_align(_action_badge, LV_ALIGN_TOP_MID, 0, 110);

    // 4. Scrollable Flex Dialogue Card
    _dialog_card = lv_obj_create(_bg_obj);
    lv_obj_set_size(_dialog_card, 372, 272);
    lv_obj_align(_dialog_card, LV_ALIGN_TOP_MID, 0, 134);
    lv_obj_set_style_bg_color(_dialog_card, lv_color_hex(0x12161F), 0);
    lv_obj_set_style_border_color(_dialog_card, lv_color_hex(0x2A3242), 0);
    lv_obj_set_style_border_width(_dialog_card, 1, 0);
    lv_obj_set_style_radius(_dialog_card, 16, 0);
    lv_obj_set_style_pad_all(_dialog_card, 14, 0);
    lv_obj_set_style_pad_row(_dialog_card, 12, 0);
    lv_obj_set_flex_flow(_dialog_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(_dialog_card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scrollbar_mode(_dialog_card, LV_SCROLLBAR_MODE_AUTO);

    _user_label = lv_label_create(_dialog_card);
    lv_obj_set_width(_user_label, LV_PCT(100));
    lv_label_set_long_mode(_user_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(_user_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_user_label, lv_color_hex(0x94A3B8), 0);
    lv_label_set_text(_user_label, "SEN: \"TARS, bugun cok yoruldum salonun isigini acar misin?\"");

    _tars_label = lv_label_create(_dialog_card);
    lv_obj_set_width(_tars_label, LV_PCT(100));
    lv_label_set_long_mode(_tars_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(_tars_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(_tars_label, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(_tars_label, "TARS: \"3X Turbo motor aktif Eymen. Konus, aninda cevap ve eylem hazir.\"");

    // 5. Bottom Control Row
    _btn_test = lv_btn_create(_bg_obj);
    lv_obj_set_size(_btn_test, 206, 52);
    lv_obj_align(_btn_test, LV_ALIGN_BOTTOM_LEFT, 18, -18);
    lv_obj_set_style_radius(_btn_test, 14, 0);
    lv_obj_set_style_bg_color(_btn_test, lv_color_hex(0x1E88E5), 0);
    lv_obj_set_style_shadow_width(_btn_test, 0, 0);
    lv_obj_add_event_cb(_btn_test, [](lv_event_t* e) {
        s_tars_manual_toggle = true;
        bsp_vibrate_pulse(35);
    }, LV_EVENT_CLICKED, this);

    _btn_test_label = lv_label_create(_btn_test);
    lv_obj_set_style_text_font(_btn_test_label, &lv_font_montserrat_14, 0);
    lv_label_set_text(_btn_test_label, LV_SYMBOL_AUDIO " Konusmayi Baslat");
    lv_obj_center(_btn_test_label);

    _btn_humor = lv_btn_create(_bg_obj);
    lv_obj_set_size(_btn_humor, 152, 52);
    lv_obj_align(_btn_humor, LV_ALIGN_BOTTOM_RIGHT, -18, -18);
    lv_obj_set_style_radius(_btn_humor, 14, 0);
    lv_obj_set_style_bg_color(_btn_humor, lv_color_hex(0x283040), 0);
    lv_obj_set_style_shadow_width(_btn_humor, 0, 0);
    lv_obj_add_event_cb(_btn_humor, [](lv_event_t* e) {
        AppTars* self = (AppTars*)lv_event_get_user_data(e);
        if (self) {
            int next = (self->get_humor() == 75) ? 100 : (self->get_humor() == 100) ? 40 : 75;
            self->set_humor(next);
            bsp_vibrate_pulse(40);
        }
    }, LV_EVENT_CLICKED, this);

    lv_obj_t* l_hum = lv_label_create(_btn_humor);
    lv_obj_set_style_text_font(l_hum, &lv_font_montserrat_14, 0);
    lv_label_set_text(l_hum, "Mizah Ayari");
    lv_obj_center(l_hum);

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

    if (_bg_obj) {
        lv_obj_delete_async(_bg_obj);
        _bg_obj = nullptr;
        _btn_back = nullptr;
        _params_label = nullptr;
        _status_label = nullptr;
        _level_bar = nullptr;
        _dialog_card = nullptr;
        _user_label = nullptr;
        _tars_label = nullptr;
        _action_badge = nullptr;
        _btn_humor = nullptr;
        _btn_test = nullptr;
        _btn_test_label = nullptr;
    }
    return true;
}
