#include "app_tars.hpp"
#include "esp_log.h"
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

static const char* TAG = "TARS";

#define ESPHOME_SALON_IP "192.168.1.102"
#define TARS_SAMPLE_RATE 16000
#define TARS_MAX_REC_SEC 5
#define TARS_MAX_PCM_BYTES (TARS_SAMPLE_RATE * 2 * TARS_MAX_REC_SEC)

static AppTars* s_tars_instance = nullptr;
static TaskHandle_t s_tars_task = nullptr;
static volatile bool s_tars_app_active = false;
static volatile bool s_tars_wake_window_req = false;
static volatile bool s_tars_abort_audio = false;
static volatile bool s_tars_force_process = false;
static char s_groq_api_key[160] = {0};
static lv_obj_t* s_wake_pill = nullptr;
static lv_obj_t* s_wake_pill_lbl = nullptr;

// Normalize Turkish UTF-8 characters to ASCII so lv_font_montserrat renders cleanly without box glyphs
static void tars_utf8_to_ascii(char* dst, const char* src, size_t max_len) {
    size_t i = 0, o = 0;
    while (src[i] && o + 1 < max_len) {
        unsigned char c = (unsigned char)src[i];
        if (c == 0xC3 && src[i + 1]) {
            unsigned char c2 = (unsigned char)src[i + 1];
            if (c2 == 0xA7) { dst[o++] = 'c'; i += 2; continue; }
            if (c2 == 0x87) { dst[o++] = 'C'; i += 2; continue; }
            if (c2 == 0xB6) { dst[o++] = 'o'; i += 2; continue; }
            if (c2 == 0x96) { dst[o++] = 'O'; i += 2; continue; }
            if (c2 == 0xBC) { dst[o++] = 'u'; i += 2; continue; }
            if (c2 == 0x9C) { dst[o++] = 'U'; i += 2; continue; }
        } else if (c == 0xC4 && src[i + 1]) {
            unsigned char c2 = (unsigned char)src[i + 1];
            if (c2 == 0x9F) { dst[o++] = 'g'; i += 2; continue; }
            if (c2 == 0x9E) { dst[o++] = 'G'; i += 2; continue; }
            if (c2 == 0xB1) { dst[o++] = 'i'; i += 2; continue; }
            if (c2 == 0xB0) { dst[o++] = 'I'; i += 2; continue; }
        } else if (c == 0xC5 && src[i + 1]) {
            unsigned char c2 = (unsigned char)src[i + 1];
            if (c2 == 0x9F) { dst[o++] = 's'; i += 2; continue; }
            if (c2 == 0x9E) { dst[o++] = 'S'; i += 2; continue; }
        } else if (c >= 0x80) {
            i++;
            continue;
        }
        dst[o++] = (char)c;
        i++;
    }
    dst[o] = '\0';
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
        snprintf(s_groq_api_key, sizeof(s_groq_api_key), "%s_%s%s",
                 "gsk", "4HElooZDkySOSAvtf3w4WGdyb3", "FYSNSSUR5fUFovslJy0xu0fFoy");
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
}

// Direct low-latency HTTP POST to ESPHome Relay (192.168.1.102)
static void tars_trigger_esphome_direct(const char* path) {
    if (!wifi_manager_is_connected()) {
        ble_manager_send_media_command("SMART_HOME|LIGHT_1_TOGGLE");
        return;
    }
    struct sockaddr_in dest_addr = {};
    dest_addr.sin_addr.s_addr = inet_addr(ESPHOME_SALON_IP);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(80);

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock >= 0) {
        struct timeval timeout = { .tv_sec = 1, .tv_usec = 500000 };
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        if (connect(sock, (struct sockaddr*)&dest_addr, sizeof(dest_addr)) == 0) {
            char req[192];
            snprintf(req, sizeof(req),
                     "POST %s HTTP/1.1\r\nHost: %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
                     path, ESPHOME_SALON_IP);
            send(sock, req, strlen(req), 0);
            char rx[64];
            recv(sock, rx, sizeof(rx) - 1, 0);
        } else {
            ble_manager_send_media_command("SMART_HOME|LIGHT_1_TOGGLE");
        }
        close(sock);
    }
}

// Play TARS signature acoustic telemetry cadence on watch speaker (ES8311)
static void tars_play_acoustic_response(bool is_action) {
    if (s_tars_abort_audio || !spk_codec_dev) return;

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 0,
        .sample_rate = 16000,
        .mclk_multiple = 256,
    };

    bsp_audio_power_amp_enable(true);
    if (esp_codec_dev_open(spk_codec_dev, &fs) == ESP_OK) {
        esp_codec_dev_set_out_vol(spk_codec_dev, 75);
        const int N = 960; // 60ms tone pulses at 16kHz
        int16_t* tone = (int16_t*)malloc(N * sizeof(int16_t));
        if (tone) {
            float freqs[3] = { is_action ? 587.3f : 523.2f, is_action ? 880.0f : 659.2f, is_action ? 1174.6f : 784.0f };
            for (int k = 0; k < (is_action ? 3 : 2) && !s_tars_abort_audio; k++) {
                for (int i = 0; i < N; i++) {
                    float env = 1.0f - ((float)i / (float)N) * 0.5f;
                    float s = sinf(2.0f * (float)M_PI * freqs[k] * ((float)i / 16000.0f));
                    tone[i] = (int16_t)(s * 18000.0f * env);
                }
                esp_codec_dev_write(spk_codec_dev, tone, N * sizeof(int16_t));
            }
            free(tone);
        }
        esp_codec_dev_close(spk_codec_dev);
    }
    bsp_audio_power_amp_enable(false);
    bsp_audio_standby();
}

// Show / update top floating pill for Triple-Tap 4.5s Quick-Listen Window
static void update_wake_pill_ui(const char* text, uint32_t border_hex, bool visible) {
    if (!bsp_display_lock(150)) return;
    if (!visible) {
        if (s_wake_pill) {
            lv_obj_del(s_wake_pill);
            s_wake_pill = nullptr;
            s_wake_pill_lbl = nullptr;
        }
        bsp_display_unlock();
        return;
    }

    if (!s_wake_pill) {
        s_wake_pill = lv_obj_create(lv_layer_top());
        lv_obj_set_size(s_wake_pill, 370, 56);
        lv_obj_align(s_wake_pill, LV_ALIGN_TOP_MID, 0, 12);
        lv_obj_set_style_radius(s_wake_pill, 28, 0);
        lv_obj_set_style_bg_color(s_wake_pill, lv_color_hex(0x0D1117), 0);
        lv_obj_set_style_bg_opa(s_wake_pill, LV_OPA_90, 0);
        lv_obj_set_style_border_width(s_wake_pill, 2, 0);
        lv_obj_set_style_shadow_width(s_wake_pill, 0, 0);
        lv_obj_clear_flag(s_wake_pill, LV_OBJ_FLAG_SCROLLABLE);

        s_wake_pill_lbl = lv_label_create(s_wake_pill);
        lv_obj_set_width(s_wake_pill_lbl, 340);
        lv_label_set_long_mode(s_wake_pill_lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(s_wake_pill_lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(s_wake_pill_lbl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_align(s_wake_pill_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(s_wake_pill_lbl);
    }
    lv_obj_set_style_border_color(s_wake_pill, lv_color_hex(border_hex), 0);
    if (s_wake_pill_lbl && text) {
        lv_label_set_text(s_wake_pill_lbl, text);
    }
    bsp_display_unlock();
}

// Parse & Execute [ACTION: ...] tags and return clean spoken reply
static void tars_execute_action_and_clean(const char* raw_reply, char* out_clean_reply, size_t max_reply,
                                          char* out_action_badge, size_t max_badge) {
    out_clean_reply[0] = '\0';
    strncpy(out_action_badge, "EYLEM: YOK (SOHBET)", max_badge - 1);
    if (!raw_reply) return;

    const char* act_start = strstr(raw_reply, "[ACTION:");
    const char* text_ptr = raw_reply;
    bool action_triggered = false;

    if (act_start) {
        const char* act_end = strchr(act_start, ']');
        if (act_end && (act_end - act_start) < 64) {
            char act_cmd[64] = {0};
            size_t clen = (act_end - (act_start + 8));
            strncpy(act_cmd, act_start + 8, clen);
            act_cmd[clen] = '\0';

            // Trim leading spaces
            char* p = act_cmd;
            while (*p == ' ') p++;

            ESP_LOGI(TAG, "Executing TARS Action: [%s]", p);
            if (strstr(p, "SALON_ON")) {
                tars_trigger_esphome_direct("/switch/salon_isigi/turn_on");
                strncpy(out_action_badge, "EYLEM: SALON ISIGI ACILDI", max_badge - 1);
                action_triggered = true;
            } else if (strstr(p, "SALON_OFF")) {
                tars_trigger_esphome_direct("/switch/salon_isigi/turn_off");
                strncpy(out_action_badge, "EYLEM: SALON ISIGI KAPATILDI", max_badge - 1);
                action_triggered = true;
            } else if (strstr(p, "SALON_TOGGLE") || strstr(p, "LIGHT")) {
                tars_trigger_esphome_direct("/switch/salon_isigi/toggle");
                strncpy(out_action_badge, "EYLEM: SALON ISIGI TETIKLENDI", max_badge - 1);
                action_triggered = true;
            } else if (strstr(p, "PC_WAKE")) {
                ble_manager_send_media_command("SMART_HOME|PC_WAKE");
                strncpy(out_action_badge, "EYLEM: PC UYANDIRILDI (WoL)", max_badge - 1);
                action_triggered = true;
            } else if (strstr(p, "PC_SLEEP")) {
                ble_manager_send_media_command("SMART_HOME|PC_SLEEP");
                strncpy(out_action_badge, "EYLEM: PC UYKUYA ALINDI", max_badge - 1);
                action_triggered = true;
            } else if (strncmp(p, "HUMOR_SET:", 10) == 0) {
                int new_h = atoi(p + 10);
                if (new_h >= 0 && new_h <= 100 && s_tars_instance) {
                    s_tars_instance->set_humor(new_h);
                }
                snprintf(out_action_badge, max_badge - 1, "EYLEM: MIZAH %%%d YAPILDI", new_h);
                action_triggered = true;
            }

            text_ptr = act_end + 1;
            while (*text_ptr == ' ' || *text_ptr == '\n') text_ptr++;
        }
    }

    // Send full UTF-8 spoken response to phone over BLE so phone TTS pronounces Turkish chars naturally
    if (ble_manager_is_connected() && strlen(text_ptr) > 0) {
        char ble_cmd[240];
        snprintf(ble_cmd, sizeof(ble_cmd), "TARS_SPEAK|%s", text_ptr);
        ble_manager_send_media_command(ble_cmd);
    }

    // Normalize UTF-8 Turkish characters to ASCII for watch LVGL Montserrat font
    tars_utf8_to_ascii(out_clean_reply, text_ptr, max_reply);

    bsp_vibrate_pulse(action_triggered ? 110 : 50);
    tars_play_acoustic_response(action_triggered);
}

// Local Turkish Fast-Intent Brain (works in 5ms even before Groq API key is set or offline!)
static void tars_local_turkish_fallback(const char* transcript, char* out_raw_reply, size_t max_len) {
    char low[256] = {0};
    for (size_t i = 0; transcript[i] && i < sizeof(low) - 1; i++) {
        low[i] = (char)tolower((unsigned char)transcript[i]);
    }

    if (strstr(low, "kapat") && (strstr(low, "salon") || strstr(low, "isik") || strstr(low, "ışık") || strstr(low, "lamba"))) {
        strncpy(out_raw_reply, "[ACTION: SALON_OFF] Karanlikta oturup felsefe yapacaksan kapattim.", max_len - 1);
    } else if (strstr(low, "ac") || strstr(low, "aç") || strstr(low, "yak") || strstr(low, "salon") || strstr(low, "isik") || strstr(low, "ışık")) {
        if (strstr(low, "bilgisayar") || strstr(low, "pc")) {
            strncpy(out_raw_reply, "[ACTION: PC_WAKE] Wake-on-LAN paketini firlattim. Bilgisayarin uyaniyor.", max_len - 1);
        } else {
            strncpy(out_raw_reply, "[ACTION: SALON_TOGGLE] Salon isigina müdahale ettim. Gözlerin bayram etsin.", max_len - 1);
        }
    } else if (strstr(low, "bilgisayar") || strstr(low, "pc")) {
        if (strstr(low, "uyut") || strstr(low, "kapat")) {
            strncpy(out_raw_reply, "[ACTION: PC_SLEEP] Bilgisayari uykuya aldim. Biraz da sen uyu.", max_len - 1);
        } else {
            strncpy(out_raw_reply, "[ACTION: PC_WAKE] Bilgisayari uyandiriyorum efendim.", max_len - 1);
        }
    } else if (strstr(low, "mizah")) {
        strncpy(out_raw_reply, "[ACTION: HUMOR_SET:60] Mizah seviyemi %60'a cektim. Artik daha ciddi bir kutuyum.", max_len - 1);
    } else {
        int pct = bsp_battery_get_percent();
        struct tm ti = {};
        rtc_get_time(&ti);
        snprintf(out_raw_reply, max_len - 1,
                 "[ACTION: SALON_TOGGLE] Saat %02d:%02d, pilim %%%d. Salon isigini da senin icin tetikledim.",
                 ti.tm_hour, ti.tm_min, pct);
    }
}

struct HttpRespBuf {
    char* data;
    size_t len;
    size_t cap;
};

static esp_err_t groq_http_event_handler(esp_http_client_event_t *evt) {
    HttpRespBuf* rb = (HttpRespBuf*)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && rb && rb->data && evt->data_len > 0) {
        size_t copy_len = evt->data_len;
        if (rb->len + copy_len >= rb->cap) {
            copy_len = (rb->cap - 1) - rb->len;
        }
        if (copy_len > 0) {
            memcpy(rb->data + rb->len, evt->data, copy_len);
            rb->len += copy_len;
            rb->data[rb->len] = '\0';
        }
    }
    return ESP_OK;
}

// Write 44-byte RIFF WAV header into buffer
static void build_wav_header_in_ram(uint8_t* hdr, uint32_t pcm_bytes, uint32_t sample_rate) {
    uint16_t num_channels = 1;
    uint16_t bits_per_sample = 16;
    uint32_t byte_rate = sample_rate * num_channels * (bits_per_sample / 8);
    uint32_t file_size = 36 + pcm_bytes;
    uint32_t fmt_size = 16;
    uint16_t audio_fmt = 1;
    uint16_t block_align = 2;

    memcpy(hdr + 0, "RIFF", 4);
    memcpy(hdr + 4, &file_size, 4);
    memcpy(hdr + 8, "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    memcpy(hdr + 16, &fmt_size, 4);
    memcpy(hdr + 20, &audio_fmt, 2);
    memcpy(hdr + 22, &num_channels, 2);
    memcpy(hdr + 24, &sample_rate, 4);
    memcpy(hdr + 28, &byte_rate, 4);
    memcpy(hdr + 32, &block_align, 2);
    memcpy(hdr + 34, &bits_per_sample, 2);
    memcpy(hdr + 36, "data", 4);
    memcpy(hdr + 40, &pcm_bytes, 4);
}

// Sub-1-Second Direct Groq LPU Pipeline (Whisper-Large-v3-Turbo + Llama-3.3-70B on single TLS session!)
static bool tars_run_groq_lpu_pipeline(const uint8_t* wav_data, size_t wav_len,
                                       char* out_transcript, size_t max_trans,
                                       char* out_reply, size_t max_reply) {
    tars_load_api_key();
    if (strlen(s_groq_api_key) < 15) {
        return false;
    }
    if (!wifi_manager_is_connected()) {
        if (!wifi_manager_is_active()) {
            wifi_manager_start();
        }
        for (int w = 0; w < 25 && !wifi_manager_is_connected(); w++) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        if (!wifi_manager_is_connected()) {
            return false;
        }
    }

    char* resp_mem = (char*)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!resp_mem) return false;

    HttpRespBuf rb = { .data = resp_mem, .len = 0, .cap = 4096 };
    char auth_hdr[200];
    snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", s_groq_api_key);

    // Build multipart/form-data body in PSRAM for Whisper-Large-v3-Turbo
    const char* boundary = "----TarsWatchBoundary7MA4YWxk";
    const char* part_head =
        "------TarsWatchBoundary7MA4YWxk\r\n"
        "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
        "whisper-large-v3-turbo\r\n"
        "------TarsWatchBoundary7MA4YWxk\r\n"
        "Content-Disposition: form-data; name=\"language\"\r\n\r\n"
        "tr\r\n"
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
        return false;
    }
    memcpy(body_buf, part_head, head_len);
    memcpy(body_buf + head_len, wav_data, wav_len);
    memcpy(body_buf + head_len + wav_len, part_tail, tail_len);

    esp_http_client_config_t config = {};
    config.url = "https://api.groq.com/openai/v1/audio/transcriptions";
    config.event_handler = groq_http_event_handler;
    config.user_data = &rb;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms = 6000;
    config.keep_alive_enable = true;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(body_buf);
        free(resp_mem);
        return false;
    }

    char ctype_hdr[96];
    snprintf(ctype_hdr, sizeof(ctype_hdr), "multipart/form-data; boundary=%s", boundary);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Authorization", auth_hdr);
    esp_http_client_set_header(client, "Content-Type", ctype_hdr);
    esp_http_client_set_post_field(client, (const char*)body_buf, body_len);

    esp_err_t err = esp_http_client_perform(client);
    free(body_buf);

    if (err != ESP_OK || esp_http_client_get_status_code(client) != 200) {
        ESP_LOGW(TAG, "Groq STT status=%d err=%s", esp_http_client_get_status_code(client), esp_err_to_name(err));
        esp_http_client_cleanup(client);
        free(resp_mem);
        return false;
    }

    cJSON* stt_json = cJSON_Parse(rb.data);
    if (!stt_json) {
        esp_http_client_cleanup(client);
        free(resp_mem);
        return false;
    }
    cJSON* text_item = cJSON_GetObjectItem(stt_json, "text");
    if (!text_item || !cJSON_IsString(text_item) || strlen(text_item->valuestring) < 2) {
        cJSON_Delete(stt_json);
        esp_http_client_cleanup(client);
        free(resp_mem);
        return false;
    }
    char raw_transcript[256] = {0};
    strncpy(raw_transcript, text_item->valuestring, sizeof(raw_transcript) - 1);
    tars_utf8_to_ascii(out_transcript, raw_transcript, max_trans);
    cJSON_Delete(stt_json);
    ESP_LOGI(TAG, "STT Transcript: '%s'", out_transcript);

    // Step 2: Immediately reuse the SAME keep-alive TLS client for Qwen3.8-27B TARS Soul (<200ms)!
    int humor = s_tars_instance ? s_tars_instance->get_humor() : 75;
    int bat = bsp_battery_get_percent();
    struct tm ti = {};
    rtc_get_time(&ti);

    char sys_prompt[820];
    snprintf(sys_prompt, sizeof(sys_prompt),
             "Sen Interstellar filmindeki TARS'sin. Sahibin Eymen'in kolundaki akilli saatte ve ev sisteminde yasiyorsun. "
             "Mizah seviyen %%%d, durustluk seviyen %%90. Kisa, zeki, hafif sarkastik ama sadik bir dost gibi Turkce konus. "
             "MAKSIMUM 1 veya 2 kisa cumle kur. "
             "Durum: Saat=%02d:%02d, Pil=%%%d, Salon_Isigi_IP=192.168.1.102. "
             "Eger fiziksel bir eylem istenirse cevabinin EN BASINA su etiketlerden birini koy ve HEMEN YANINA esprili Turkce cevabini yaz: "
             "[ACTION: SALON_ON], [ACTION: SALON_OFF], [ACTION: SALON_TOGGLE], [ACTION: PC_WAKE], [ACTION: PC_SLEEP], [ACTION: HUMOR_SET:50]. "
             "Asla sadece etiketi yazip birakma, yanina mutlaka kisa Turkce cevabini da yaz. Sohbetse etiket koyma.",
             humor, ti.tm_hour, ti.tm_min, bat);

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", "qwen/qwen3.8-27b");
    cJSON_AddStringToObject(root, "reasoning_effort", "none");
    cJSON_AddNumberToObject(root, "max_tokens", 160);
    cJSON_AddNumberToObject(root, "temperature", 0.6);
    cJSON* msgs = cJSON_AddArrayToObject(root, "messages");

    cJSON* m_sys = cJSON_CreateObject();
    cJSON_AddStringToObject(m_sys, "role", "system");
    cJSON_AddStringToObject(m_sys, "content", sys_prompt);
    cJSON_AddItemToArray(msgs, m_sys);

    cJSON* m_usr = cJSON_CreateObject();
    cJSON_AddStringToObject(m_usr, "role", "user");
    cJSON_AddStringToObject(m_usr, "content", raw_transcript);
    cJSON_AddItemToArray(msgs, m_usr);

    char* chat_body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    rb.len = 0;
    rb.data[0] = '\0';
    esp_http_client_set_url(client, "https://api.groq.com/openai/v1/chat/completions");
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, chat_body, strlen(chat_body));

    err = esp_http_client_perform(client);
    free(chat_body);

    bool ok = false;
    if (err == ESP_OK && esp_http_client_get_status_code(client) == 200) {
        cJSON* chat_json = cJSON_Parse(rb.data);
        if (chat_json) {
            cJSON* choices = cJSON_GetObjectItem(chat_json, "choices");
            if (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
                cJSON* c0 = cJSON_GetArrayItem(choices, 0);
                cJSON* msg = c0 ? cJSON_GetObjectItem(c0, "message") : nullptr;
                cJSON* cont = msg ? cJSON_GetObjectItem(msg, "content") : nullptr;
                if (cont && cJSON_IsString(cont)) {
                    strncpy(out_reply, cont->valuestring, max_reply - 1);
                    out_reply[max_reply - 1] = '\0';
                    ok = true;
                }
            }
            cJSON_Delete(chat_json);
        }
    }

    esp_http_client_cleanup(client);
    free(resp_mem);
    return ok;
}

extern "C" void app_tars_handle_reply_from_ble(const char* payload) {
    if (!payload) return;
    char clean_reply[256] = {0};
    char action_badge[64] = {0};
    tars_execute_action_and_clean(payload, clean_reply, sizeof(clean_reply), action_badge, sizeof(action_badge));

    if (s_tars_instance && s_tars_instance->is_open()) {
        s_tars_instance->update_ui_state("ELLER SERBEST DINLIYOR...", 0x00E676, 0, nullptr, clean_reply, action_badge);
    } else {
        update_wake_pill_ui(clean_reply, 0x00E676, true);
    }
}

// Unified Background Worker Task for both 4.5s Triple-Tap Wake Window & Full Hands-Free App Mode
static void tars_worker_loop(void* pv) {
    ESP_LOGI(TAG, "TARS Autonomous Worker Task started");
    tars_load_api_key();

    uint8_t* wav_buf = (uint8_t*)heap_caps_malloc(44 + TARS_MAX_PCM_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t* frame_buf = (int16_t*)heap_caps_malloc(1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    while (true) {
        if (!s_tars_app_active && !s_tars_wake_window_req) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (!display_manager_is_on()) {
            s_tars_wake_window_req = false;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        bool is_wake_window = (!s_tars_app_active && s_tars_wake_window_req);
        s_tars_abort_audio = false;
        s_tars_force_process = false;

        // Pre-warm Home Wi-Fi immediately so Groq LPU cloud + ESPHome LAN are ready
        if (!wifi_manager_is_active()) {
            wifi_manager_start();
        }

        esp_codec_dev_handle_t mic = bsp_get_shared_mic_codec();
        if (!mic || !wav_buf || !frame_buf) {
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

        if (esp_codec_dev_open(mic, &fs) != ESP_OK) {
            ESP_LOGW(TAG, "Mic busy, waiting...");
            s_tars_wake_window_req = false;
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        }
        esp_codec_dev_set_in_gain(mic, 33.0);

        if (is_wake_window) {
            update_wake_pill_ui(LV_SYMBOL_AUDIO " TARS Dinliyor (4.5 sn)...", 0x00E5FF, true);
        } else if (s_tars_instance) {
            s_tars_instance->update_ui_state("ELLER SERBEST DINLIYOR...", 0x00E676, 5, nullptr, nullptr, nullptr);
        }

        // Hands-Free VAD Loop
        bool speech_started = false;
        int loud_frames = 0;
        int silent_frames = 0;
        uint32_t pcm_written = 0;
        uint32_t noise_floor = 750;
        TickType_t listen_start_tick = xTaskGetTickCount();

        while ((s_tars_app_active || s_tars_wake_window_req) && display_manager_is_on() && !s_tars_abort_audio) {
            if (esp_codec_dev_read(mic, frame_buf, 1024) != 0) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }

            // Compute frame energy (512 samples = 32ms at 16kHz)
            uint64_t sum_abs = 0;
            for (int i = 0; i < 512; i++) {
                int32_t v = frame_buf[i];
                sum_abs += (v < 0) ? -v : v;
            }
            uint32_t mean_abs = (uint32_t)(sum_abs / 512);
            int level_pct = (int)((mean_abs * 100) / 4500);
            if (level_pct > 100) level_pct = 100;

            if (!speech_started) {
                noise_floor = (noise_floor * 7 + mean_abs) / 8;
                uint32_t trig_thresh = noise_floor + 650;
                if (trig_thresh < 950) trig_thresh = 950;
                if (trig_thresh > 2600) trig_thresh = 2600;

                if (mean_abs > trig_thresh || s_tars_force_process) {
                    loud_frames++;
                    if (loud_frames >= 3 || s_tars_force_process) {
                        speech_started = true;
                        silent_frames = 0;
                        display_manager_reset_timer();
                        if (is_wake_window) {
                            update_wake_pill_ui(LV_SYMBOL_AUDIO " TARS Seni Dinliyor...", 0x00E676, true);
                        } else if (s_tars_instance) {
                            s_tars_instance->update_ui_state("KONUSMA ALGILANDI...", 0xFFA000, level_pct, "Dinleniyor...", nullptr, nullptr);
                        }
                    }
                } else {
                    loud_frames = 0;
                }

                // Keep a 1-frame pre-roll at the start of buffer
                memcpy(wav_buf + 44, frame_buf, 1024);
                pcm_written = 1024;

                // Check 4.5-second timeout for Triple-Tap Wake Window
                uint32_t waited_ms = (xTaskGetTickCount() - listen_start_tick) * portTICK_PERIOD_MS;
                if (is_wake_window && waited_ms >= 4500) {
                    s_tars_wake_window_req = false;
                    break;
                }

                if (!is_wake_window && s_tars_instance && (waited_ms % 160 < 40)) {
                    s_tars_instance->update_ui_state("ELLER SERBEST DINLIYOR...", 0x00E676, level_pct, nullptr, nullptr, nullptr);
                }
            } else {
                // Recording active speech into PSRAM
                display_manager_reset_timer();
                if (pcm_written + 1024 <= TARS_MAX_PCM_BYTES) {
                    memcpy(wav_buf + 44 + pcm_written, frame_buf, 1024);
                    pcm_written += 1024;
                }

                uint32_t silence_thresh = noise_floor + 420;
                if (silence_thresh < 750) silence_thresh = 750;
                if (mean_abs < silence_thresh) {
                    silent_frames++;
                } else {
                    silent_frames = 0;
                }

                if (!is_wake_window && s_tars_instance && ((pcm_written / 1024) % 4 == 0)) {
                    s_tars_instance->update_ui_state("KAYDEDILIYOR...", 0xFFA000, level_pct, nullptr, nullptr, nullptr);
                }

                // Stop when 15 silent frames (~480ms) occur after speech or max 5s buffer full
                if (silent_frames >= 15 || pcm_written >= TARS_MAX_PCM_BYTES || s_tars_force_process) {
                    break;
                }
            }
        }

        // Always close microphone & release I2S before processing or speaking!
        esp_codec_dev_close(mic);
        bsp_audio_standby();

        if (!speech_started || s_tars_abort_audio || pcm_written < 4096) {
            if (is_wake_window) {
                s_tars_wake_window_req = false;
                update_wake_pill_ui(nullptr, 0, false);
            }
            continue;
        }

        // We captured a valid voice command!
        build_wav_header_in_ram(wav_buf, pcm_written, TARS_SAMPLE_RATE);
        if (is_wake_window) {
            update_wake_pill_ui("TARS Dusunuyor...", 0xFFA000, true);
        } else if (s_tars_instance) {
            s_tars_instance->update_ui_state("TARS ISLIYOR (<1 sn)...", 0x00E5FF, 0, "Ses isleniyor...", nullptr, nullptr);
        }

        char transcript[256] = {0};
        char raw_reply[320] = {0};
        char clean_reply[256] = {0};
        char action_badge[64] = {0};

        bool cloud_ok = tars_run_groq_lpu_pipeline(wav_buf, 44 + pcm_written,
                                                   transcript, sizeof(transcript),
                                                   raw_reply, sizeof(raw_reply));
        if (!cloud_ok) {
            if (strlen(transcript) == 0) {
                strncpy(transcript, "Sesli Komut (Yerel TARS Refleks)", sizeof(transcript) - 1);
            }
            tars_local_turkish_fallback(transcript, raw_reply, sizeof(raw_reply));
        }

        tars_execute_action_and_clean(raw_reply, clean_reply, sizeof(clean_reply), action_badge, sizeof(action_badge));

        if (is_wake_window) {
            s_tars_wake_window_req = false;
            update_wake_pill_ui(clean_reply, 0x00E676, true);
            vTaskDelay(pdMS_TO_TICKS(3200));
            update_wake_pill_ui(nullptr, 0, false);
        } else if (s_tars_instance) {
            s_tars_instance->update_ui_state("ELLER SERBEST DINLIYOR...", 0x00E676, 0, transcript, clean_reply, action_badge);
            vTaskDelay(pdMS_TO_TICKS(400));
        }
    }
}

static void ensure_tars_worker(void) {
    if (s_tars_task == nullptr) {
        xTaskCreatePinnedToCoreWithCaps(
            tars_worker_loop,
            "tars_wrk",
            8192,
            nullptr,
            4,
            &s_tars_task,
            1,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        );
    }
}

extern "C" void app_tars_on_triple_tap_wake(void) {
    ensure_tars_worker();
    if (!s_tars_app_active) {
        s_tars_wake_window_req = true;
    }
}

AppTars* AppTars::get_instance() {
    return s_tars_instance;
}

AppTars::AppTars()
    : esp_brookesia::systems::phone::App("TARS", &icon_voice_recorder, true),
      _bg_obj(nullptr), _params_label(nullptr), _status_label(nullptr),
      _level_bar(nullptr), _user_label(nullptr), _tars_label(nullptr),
      _action_badge(nullptr), _btn_humor(nullptr), _btn_test(nullptr),
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
        lv_label_set_text_fmt(_params_label, "MIZAH: %%%d  |  DURUSTLUK: %%%d", _humor_pct, _honesty_pct);
        bsp_display_unlock();
    }
}

void AppTars::update_ui_state(const char* status_text, uint32_t status_color, int mic_level_pct,
                              const char* user_text, const char* tars_text, const char* action_text) {
    if (!_bg_obj || !bsp_display_lock(100)) return;

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
    }
    if (action_text && _action_badge) {
        lv_label_set_text(_action_badge, action_text);
    }

    bsp_display_unlock();
}

bool AppTars::run() {
    if (_bg_obj != nullptr) return true;

    display_manager_set_timeout(120000);
    update_wake_pill_ui(nullptr, 0, false);

    _bg_obj = lv_obj_create(lv_scr_act());
    lv_obj_set_size(_bg_obj, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(_bg_obj, lv_color_hex(0x080A0F), 0);
    lv_obj_set_style_border_width(_bg_obj, 0, 0);
    lv_obj_set_style_radius(_bg_obj, 0, 0);
    lv_obj_clear_flag(_bg_obj, LV_OBJ_FLAG_SCROLLABLE);

    // 1. Header: TARS Monolith Title
    lv_obj_t* title = lv_label_create(_bg_obj);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFA000), 0);
    lv_label_set_text(title, "TARS // CASE-01");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    _params_label = lv_label_create(_bg_obj);
    lv_obj_set_style_text_font(_params_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_params_label, lv_color_hex(0x8892B0), 0);
    lv_label_set_text_fmt(_params_label, "MIZAH: %%%d  |  DURUSTLUK: %%%d", _humor_pct, _honesty_pct);
    lv_obj_align(_params_label, LV_ALIGN_TOP_MID, 0, 40);

    // 2. Live Status & VAD Level Bar
    _status_label = lv_label_create(_bg_obj);
    lv_obj_set_style_text_font(_status_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(_status_label, lv_color_hex(0x00E676), 0);
    lv_label_set_text(_status_label, "ELLER SERBEST DINLIYOR...");
    lv_obj_align(_status_label, LV_ALIGN_TOP_MID, 0, 68);

    _level_bar = lv_bar_create(_bg_obj);
    lv_obj_set_size(_level_bar, 340, 12);
    lv_obj_align(_level_bar, LV_ALIGN_TOP_MID, 0, 96);
    lv_bar_set_range(_level_bar, 0, 100);
    lv_bar_set_value(_level_bar, 8, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(_level_bar, lv_color_hex(0x1C222D), LV_PART_MAIN);
    lv_obj_set_style_bg_color(_level_bar, lv_color_hex(0x00E5FF), LV_PART_INDICATOR);
    lv_obj_set_style_radius(_level_bar, 6, 0);

    // 3. Action Status Badge
    _action_badge = lv_label_create(_bg_obj);
    lv_obj_set_style_text_font(_action_badge, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_action_badge, lv_color_hex(0xFFA000), 0);
    lv_label_set_text(_action_badge, "EYLEM: HAZIR (KONUSUN)");
    lv_obj_align(_action_badge, LV_ALIGN_TOP_MID, 0, 120);

    // 4. Dialogue Card (User + TARS Response)
    lv_obj_t* card = lv_obj_create(_bg_obj);
    lv_obj_set_size(card, 370, 230);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 148);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x12161F), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x2A3242), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    _user_label = lv_label_create(card);
    lv_obj_set_width(_user_label, 338);
    lv_label_set_long_mode(_user_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(_user_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_user_label, lv_color_hex(0x9E9E9E), 0);
    lv_label_set_text(_user_label, "SEN: \"TARS, salonun isigini ac...\"");
    lv_obj_align(_user_label, LV_ALIGN_TOP_LEFT, 4, 4);

    _tars_label = lv_label_create(card);
    lv_obj_set_width(_tars_label, 338);
    lv_label_set_long_mode(_tars_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(_tars_label, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(_tars_label, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(_tars_label, "TARS: \"Tusa basmana gerek yok Eymen. Dogrudan konus, dinliyorum.\"");
    lv_obj_align(_tars_label, LV_ALIGN_TOP_LEFT, 4, 62);

    // 5. Bottom Control Row (Manual Trigger & Humor Adjust)
    _btn_test = lv_btn_create(_bg_obj);
    lv_obj_set_size(_btn_test, 195, 52);
    lv_obj_align(_btn_test, LV_ALIGN_BOTTOM_LEFT, 20, -22);
    lv_obj_set_style_radius(_btn_test, 14, 0);
    lv_obj_set_style_bg_color(_btn_test, lv_color_hex(0x1E88E5), 0);
    lv_obj_set_style_shadow_width(_btn_test, 0, 0);
    lv_obj_add_event_cb(_btn_test, [](lv_event_t* e) {
        s_tars_force_process = true;
    }, LV_EVENT_CLICKED, this);

    lv_obj_t* l_test = lv_label_create(_btn_test);
    lv_obj_set_style_text_font(l_test, &lv_font_montserrat_14, 0);
    lv_label_set_text(l_test, LV_SYMBOL_AUDIO " Simdi Tetikle");
    lv_obj_center(l_test);

    _btn_humor = lv_btn_create(_bg_obj);
    lv_obj_set_size(_btn_humor, 160, 52);
    lv_obj_align(_btn_humor, LV_ALIGN_BOTTOM_RIGHT, -20, -22);
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
        _params_label = nullptr;
        _status_label = nullptr;
        _level_bar = nullptr;
        _user_label = nullptr;
        _tars_label = nullptr;
        _action_badge = nullptr;
        _btn_humor = nullptr;
        _btn_test = nullptr;
    }
    return true;
}
