#include "tv_control.hpp"
#include "tv_certs_local.h"
#include "pc_control.hpp"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/pk.h"
#include "mbedtls/error.h"

#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

extern "C" {
#include "wifi_manager.h"
}

static const char* TAG = "TV_CTRL";

#define TV_DEFAULT_IP       "192.168.1.159"
#define TV_REMOTE_PORT      "6466"
#define TV_IDLE_CLOSE_MS    25000

// Standard Android KeyCodes (remotemessage.proto)
enum TvKeyCode {
    TV_KEY_HOME         = 3,
    TV_KEY_BACK         = 4,
    TV_KEY_DPAD_UP      = 19,
    TV_KEY_DPAD_DOWN    = 20,
    TV_KEY_DPAD_LEFT    = 21,
    TV_KEY_DPAD_RIGHT   = 22,
    TV_KEY_DPAD_CENTER  = 23,
    TV_KEY_VOL_UP       = 24,
    TV_KEY_VOL_DOWN     = 25,
    TV_KEY_POWER        = 26,
    TV_KEY_MENU         = 82,
    TV_KEY_PLAY_PAUSE   = 85,
    TV_KEY_MEDIA_NEXT   = 87,
    TV_KEY_MEDIA_PREV   = 88,
    TV_KEY_MEDIA_REW    = 89,
    TV_KEY_MEDIA_FWD    = 90,
    TV_KEY_MEDIA_PLAY   = 126,
    TV_KEY_MEDIA_PAUSE  = 127,
    TV_KEY_MUTE         = 164,
    TV_KEY_CH_UP        = 166,
    TV_KEY_CH_DOWN      = 167,
    TV_KEY_SLEEP        = 223,
    TV_KEY_WAKEUP       = 224,
};

struct TvAsyncCmd {
    char cmd[192];
};

struct TvTlsSession {
    mbedtls_net_context net;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt client_cert;
    mbedtls_pk_context client_key;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    bool initialized;
    bool connected;
    int64_t last_activity_us;
};

static TvTlsSession* s_tv = nullptr;
static SemaphoreHandle_t s_tv_mutex = nullptr;
static QueueHandle_t s_tv_queue = nullptr;
static TaskHandle_t s_tv_task = nullptr;

const char* tv_control_get_ip(void) {
    return TV_DEFAULT_IP;
}

static void ensure_tv_mutex(void) {
    if (!s_tv_mutex) {
        s_tv_mutex = xSemaphoreCreateMutex();
    }
}

// ---------------------------------------------------------------------------
// Minimal Zero-Allocation Protobuf Encoder / Decoder for Android TV Remote v2
// ---------------------------------------------------------------------------
static size_t pb_put_varint(uint8_t* dst, uint32_t val) {
    size_t n = 0;
    do {
        uint8_t b = val & 0x7F;
        val >>= 7;
        if (val) b |= 0x80;
        dst[n++] = b;
    } while (val);
    return n;
}

static size_t pb_put_field_varint(uint8_t* dst, uint32_t field_num, uint32_t val) {
    size_t n = pb_put_varint(dst, (field_num << 3) | 0);
    n += pb_put_varint(dst + n, val);
    return n;
}

static size_t pb_put_field_bytes(uint8_t* dst, uint32_t field_num, const uint8_t* data, size_t len) {
    size_t n = pb_put_varint(dst, (field_num << 3) | 2);
    n += pb_put_varint(dst + n, (uint32_t)len);
    if (len > 0) memcpy(dst + n, data, len);
    return n + len;
}

static size_t pb_put_field_str(uint8_t* dst, uint32_t field_num, const char* str) {
    return pb_put_field_bytes(dst, field_num, (const uint8_t*)str, strlen(str));
}

static bool pb_read_varint(const uint8_t* buf, size_t len, size_t* pos, uint32_t* out_val) {
    uint32_t val = 0;
    int shift = 0;
    while (*pos < len && shift < 35) {
        uint8_t b = buf[(*pos)++];
        val |= (uint32_t)(b & 0x7F) << shift;
        if ((b & 0x80) == 0) {
            *out_val = val;
            return true;
        }
        shift += 7;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Framed TLS Transport Helpers (Varint Length Prefix)
// ---------------------------------------------------------------------------
static bool tv_ssl_write_all(TvTlsSession* s, const uint8_t* buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int r = mbedtls_ssl_write(&s->ssl, buf + sent, len - sent);
        if (r > 0) {
            sent += (size_t)r;
        } else if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
            vTaskDelay(pdMS_TO_TICKS(5));
        } else {
            return false;
        }
    }
    return true;
}

static bool tv_send_framed(TvTlsSession* s, const uint8_t* payload, size_t len) {
    uint8_t hdr[8];
    size_t hlen = pb_put_varint(hdr, (uint32_t)len);
    if (!tv_ssl_write_all(s, hdr, hlen)) return false;
    if (len > 0 && !tv_ssl_write_all(s, payload, len)) return false;
    s->last_activity_us = esp_timer_get_time();
    return true;
}

static bool tv_ssl_read_exact(TvTlsSession* s, uint8_t* dst, size_t len, int timeout_ms) {
    struct timeval tv = { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };
    setsockopt(s->net.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    size_t got = 0;
    while (got < len) {
        int r = mbedtls_ssl_read(&s->ssl, dst + got, len - got);
        if (r > 0) {
            got += (size_t)r;
        } else if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
            vTaskDelay(pdMS_TO_TICKS(5));
        } else {
            return false;
        }
    }
    return true;
}

static int tv_recv_framed(TvTlsSession* s, uint8_t* out_buf, size_t max_len, int timeout_ms) {
    uint32_t msg_len = 0;
    int shift = 0;
    while (shift < 28) {
        uint8_t b = 0;
        if (!tv_ssl_read_exact(s, &b, 1, timeout_ms)) return -1;
        msg_len |= (uint32_t)(b & 0x7F) << shift;
        if ((b & 0x80) == 0) break;
        shift += 7;
    }
    if (msg_len == 0) return 0;
    if (msg_len <= max_len) {
        if (!tv_ssl_read_exact(s, out_buf, msg_len, timeout_ms)) return -1;
        return (int)msg_len;
    }
    // Discard oversized incoming packet cleanly so stream stays synchronized
    uint8_t dump[64];
    size_t rem = msg_len;
    while (rem > 0) {
        size_t chunk = (rem > sizeof(dump)) ? sizeof(dump) : rem;
        if (!tv_ssl_read_exact(s, dump, chunk, timeout_ms)) return -1;
        rem -= chunk;
    }
    return 0;
}

// Handle incoming RemoteMessage (answers RemotePingRequest field 8 automatically)
static uint32_t tv_inspect_and_handle_rx(TvTlsSession* s, const uint8_t* data, size_t len) {
    size_t pos = 0;
    uint32_t first_field = 0;
    while (pos < len) {
        uint32_t tag = 0;
        if (!pb_read_varint(data, len, &pos, &tag)) break;
        uint32_t fnum = tag >> 3;
        uint32_t wtype = tag & 7;
        if (first_field == 0) first_field = fnum;
        if (wtype == 0) {
            uint32_t v = 0;
            if (!pb_read_varint(data, len, &pos, &v)) break;
        } else if (wtype == 2) {
            uint32_t flen = 0;
            if (!pb_read_varint(data, len, &pos, &flen)) break;
            if (pos + flen > len) break;
            if (fnum == 8) {
                // RemotePingRequest { int32 val1 = 1; } -> Reply RemotePingResponse (field 9)
                uint32_t val1 = 0;
                size_t p2 = 0;
                const uint8_t* sub = data + pos;
                while (p2 < flen) {
                    uint32_t t2 = 0;
                    if (!pb_read_varint(sub, flen, &p2, &t2)) break;
                    if ((t2 >> 3) == 1 && (t2 & 7) == 0) {
                        pb_read_varint(sub, flen, &p2, &val1);
                        break;
                    } else if ((t2 & 7) == 0) {
                        uint32_t dummy = 0;
                        pb_read_varint(sub, flen, &p2, &dummy);
                    } else if ((t2 & 7) == 2) {
                        uint32_t l2 = 0;
                        if (!pb_read_varint(sub, flen, &p2, &l2)) break;
                        p2 += l2;
                    } else {
                        break;
                    }
                }
                uint8_t inner[12], msg[20];
                size_t il = pb_put_field_varint(inner, 1, val1);
                size_t ml = pb_put_field_bytes(msg, 9, inner, il);
                tv_send_framed(s, msg, ml);
            }
            pos += flen;
        } else if (wtype == 5) {
            pos += 4;
        } else if (wtype == 1) {
            pos += 8;
        } else {
            break;
        }
    }
    return first_field;
}

static void tv_close_locked(void) {
    if (!s_tv) return;
    if (s_tv->connected) {
        mbedtls_ssl_close_notify(&s_tv->ssl);
        s_tv->connected = false;
    }
    if (s_tv->initialized) {
        mbedtls_net_free(&s_tv->net);
        mbedtls_ssl_free(&s_tv->ssl);
        mbedtls_ssl_config_free(&s_tv->conf);
        mbedtls_x509_crt_free(&s_tv->client_cert);
        mbedtls_pk_free(&s_tv->client_key);
        mbedtls_ctr_drbg_free(&s_tv->ctr_drbg);
        mbedtls_entropy_free(&s_tv->entropy);
        s_tv->initialized = false;
    }
    free(s_tv);
    s_tv = nullptr;
    ESP_LOGI(TAG, "TV TLS session closed cleanly");
}

static bool tv_connect_locked(void) {
    if (s_tv && s_tv->connected) return true;
    if (!wifi_manager_is_connected() || wifi_manager_is_ap_active()) return false;

    tv_close_locked();

    // Allocate TLS context in Octal PSRAM (keeps Internal SRAM completely free!)
    s_tv = (TvTlsSession*)heap_caps_calloc(1, sizeof(TvTlsSession), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_tv) s_tv = (TvTlsSession*)calloc(1, sizeof(TvTlsSession));
    if (!s_tv) return false;

    mbedtls_net_init(&s_tv->net);
    mbedtls_ssl_init(&s_tv->ssl);
    mbedtls_ssl_config_init(&s_tv->conf);
    mbedtls_x509_crt_init(&s_tv->client_cert);
    mbedtls_pk_init(&s_tv->client_key);
    mbedtls_entropy_init(&s_tv->entropy);
    mbedtls_ctr_drbg_init(&s_tv->ctr_drbg);
    s_tv->initialized = true;

    const char* pers = "uz_watch_tv";
    if (mbedtls_ctr_drbg_seed(&s_tv->ctr_drbg, mbedtls_entropy_func, &s_tv->entropy,
                              (const unsigned char*)pers, strlen(pers)) != 0) {
        ESP_LOGE(TAG, "ctr_drbg_seed failed");
        tv_close_locked();
        return false;
    }

    if (mbedtls_x509_crt_parse(&s_tv->client_cert,
                               (const unsigned char*)UZ_TV_CLIENT_CERT_PEM,
                               sizeof(UZ_TV_CLIENT_CERT_PEM)) != 0) {
        ESP_LOGE(TAG, "client cert parse failed");
        tv_close_locked();
        return false;
    }

    if (mbedtls_pk_parse_key(&s_tv->client_key,
                             (const unsigned char*)UZ_TV_CLIENT_KEY_PEM,
                             sizeof(UZ_TV_CLIENT_KEY_PEM),
                             nullptr, 0,
                             mbedtls_ctr_drbg_random, &s_tv->ctr_drbg) != 0) {
        ESP_LOGE(TAG, "client key parse failed");
        tv_close_locked();
        return false;
    }

    wifi_manager_mark_activity();
    int ret = mbedtls_net_connect(&s_tv->net, TV_DEFAULT_IP, TV_REMOTE_PORT, MBEDTLS_NET_PROTO_TCP);
    if (ret != 0) {
        ESP_LOGW(TAG, "TCP connect to %s:%s failed (-0x%04X)", TV_DEFAULT_IP, TV_REMOTE_PORT, -ret);
        tv_close_locked();
        return false;
    }

    struct timeval tv_to = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(s_tv->net.fd, SOL_SOCKET, SO_SNDTIMEO, &tv_to, sizeof(tv_to));
    setsockopt(s_tv->net.fd, SOL_SOCKET, SO_RCVTIMEO, &tv_to, sizeof(tv_to));

    if (mbedtls_ssl_config_defaults(&s_tv->conf, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        tv_close_locked();
        return false;
    }

    // Android TV uses a self-signed local cert; TV verifies OUR paired client certificate (mTLS)
    mbedtls_ssl_conf_authmode(&s_tv->conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&s_tv->conf, mbedtls_ctr_drbg_random, &s_tv->ctr_drbg);
    if (mbedtls_ssl_conf_own_cert(&s_tv->conf, &s_tv->client_cert, &s_tv->client_key) != 0) {
        ESP_LOGE(TAG, "conf_own_cert failed");
        tv_close_locked();
        return false;
    }

    if (mbedtls_ssl_setup(&s_tv->ssl, &s_tv->conf) != 0) {
        tv_close_locked();
        return false;
    }
    mbedtls_ssl_set_bio(&s_tv->ssl, &s_tv->net, mbedtls_net_send, mbedtls_net_recv, nullptr);

    while ((ret = mbedtls_ssl_handshake(&s_tv->ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            ESP_LOGW(TAG, "TLS handshake to TV failed (-0x%04X)", -ret);
            tv_close_locked();
            return false;
        }
    }

    s_tv->connected = true;
    s_tv->last_activity_us = esp_timer_get_time();

    // Perform Android TV Remote v2 Handshake: RemoteConfigure (1) + RemoteSetActive (2)
    uint8_t* rx = (uint8_t*)heap_caps_malloc(512, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rx) rx = (uint8_t*)malloc(512);
    if (!rx) {
        tv_close_locked();
        return false;
    }

    bool active_done = false;
    for (int step = 0; step < 5 && !active_done; step++) {
        int rlen = tv_recv_framed(s_tv, rx, 512, 2500);
        if (rlen < 0) {
            free(rx);
            tv_close_locked();
            return false;
        }
        if (rlen == 0) continue;
        uint32_t fnum = tv_inspect_and_handle_rx(s_tv, rx, (size_t)rlen);
        if (fnum == 1) {
            // Reply RemoteConfigure (code1=622, device_info)
            uint8_t dev[96], cfg[128], msg[144];
            size_t dl = 0;
            dl += pb_put_field_str(dev + dl, 1, "UZ WATCH");
            dl += pb_put_field_str(dev + dl, 2, "Eymen");
            dl += pb_put_field_varint(dev + dl, 3, 1);
            dl += pb_put_field_str(dev + dl, 4, "1");
            dl += pb_put_field_str(dev + dl, 5, "atvremote");
            dl += pb_put_field_str(dev + dl, 6, "1.0.0");

            size_t cl = 0;
            cl += pb_put_field_varint(cfg + cl, 1, 622);
            cl += pb_put_field_bytes(cfg + cl, 2, dev, dl);

            size_t ml = pb_put_field_bytes(msg, 1, cfg, cl);
            if (!tv_send_framed(s_tv, msg, ml)) {
                free(rx);
                tv_close_locked();
                return false;
            }
        } else if (fnum == 2) {
            // Reply RemoteSetActive (active=622)
            uint8_t act[16], msg[24];
            size_t al = pb_put_field_varint(act, 1, 622);
            size_t ml = pb_put_field_bytes(msg, 2, act, al);
            if (!tv_send_framed(s_tv, msg, ml)) {
                free(rx);
                tv_close_locked();
                return false;
            }
            active_done = true;
        }
    }

    free(rx);
    ESP_LOGI(TAG, "Direct Watch -> Tivibu AndroidTV (%s:6466) CONNECTED & READY!", TV_DEFAULT_IP);
    return true;
}

static bool tv_send_key_locked(int key_code, int repeat = 1) {
    uint8_t inner[24], msg[32];
    size_t il = 0;
    il += pb_put_field_varint(inner + il, 1, (uint32_t)key_code);
    il += pb_put_field_varint(inner + il, 2, 3); // RemoteDirection::SHORT = 3
    size_t ml = pb_put_field_bytes(msg, 10, inner, il); // field 10 = remote_key_inject

    for (int i = 0; i < repeat; i++) {
        if (!tv_send_framed(s_tv, msg, ml)) return false;
        if (repeat > 1 && i + 1 < repeat) vTaskDelay(pdMS_TO_TICKS(60));
    }
    return true;
}

static bool tv_send_app_link_locked(const char* url) {
    if (!url || strlen(url) == 0 || strlen(url) > 220) return false;
    uint8_t* inner = (uint8_t*)heap_caps_malloc(256, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t* msg = (uint8_t*)heap_caps_malloc(272, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!inner || !msg) {
        free(inner);
        free(msg);
        return false;
    }
    size_t il = pb_put_field_str(inner, 1, url);
    size_t ml = pb_put_field_bytes(msg, 90, inner, il); // field 90 = remote_app_link_launch_request
    bool ok = tv_send_framed(s_tv, msg, ml);
    free(inner);
    free(msg);
    return ok;
}

// Optional helper: ask local UZ_PC_Agent to resolve a YouTube query to exact watch?v=... URL for TV!
static bool tv_resolve_youtube_via_pc(const char* query, char* out_url, size_t max_len) {
    if (!query || !wifi_manager_is_connected()) return false;
    const char* pc_ip = pc_control_get_cached_ip();

    // Minimal percent-encoding for URL query
    char* enc = (char*)heap_caps_malloc(256, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char* req = (char*)heap_caps_malloc(384, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char* rx  = (char*)heap_caps_malloc(512, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!enc || !req || !rx) {
        free(enc); free(req); free(rx);
        return false;
    }

    size_t eo = 0;
    for (size_t i = 0; query[i] && eo + 4 < 255; i++) {
        unsigned char c = (unsigned char)query[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_') {
            enc[eo++] = (char)c;
        } else if (c == ' ') {
            enc[eo++] = '+';
        } else {
            eo += (size_t)snprintf(enc + eo, 255 - eo, "%%%02X", c);
        }
    }
    enc[eo] = '\0';

    bool ok = false;
    struct sockaddr_in dst = {};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(5000);
    dst.sin_addr.s_addr = inet_addr(pc_ip);
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock >= 0) {
        struct timeval tv = { .tv_sec = 3, .tv_usec = 500000 };
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        if (connect(sock, (struct sockaddr*)&dst, sizeof(dst)) == 0) {
            int rl = snprintf(req, 384, "GET /yt_resolve?q=%s HTTP/1.1\r\nHost: %s:5000\r\nConnection: close\r\n\r\n", enc, pc_ip);
            if (rl > 0 && send(sock, req, rl, 0) > 0) {
                memset(rx, 0, 512);
                int n = recv(sock, rx, 511, 0);
                if (n > 0) {
                    const char* k = strstr(rx, "\"url\"");
                    const char* c = k ? strchr(k + 5, ':') : nullptr;
                    const char* q1 = c ? strchr(c, '"') : nullptr;
                    const char* q2 = q1 ? strchr(q1 + 1, '"') : nullptr;
                    if (q1 && q2 && q2 > q1 + 10) {
                        size_t ulen = (size_t)(q2 - (q1 + 1));
                        if (ulen < max_len) {
                            memcpy(out_url, q1 + 1, ulen);
                            out_url[ulen] = '\0';
                            ok = true;
                        }
                    }
                }
            }
        }
        close(sock);
    }

    if (!ok) {
        snprintf(out_url, max_len, "https://www.youtube.com/results?search_query=%s", enc);
        ok = true;
    }
    free(enc); free(req); free(rx);
    return ok;
}

static bool tv_dispatch_locked(const char* cmd) {
    if (strcmp(cmd, "TV_POWER") == 0)          return tv_send_key_locked(TV_KEY_POWER);
    if (strcmp(cmd, "TV_SLEEP") == 0)          return tv_send_key_locked(TV_KEY_SLEEP);
    if (strcmp(cmd, "TV_WAKE") == 0)           return tv_send_key_locked(TV_KEY_WAKEUP);
    if (strcmp(cmd, "TV_PLAY_PAUSE") == 0)     return tv_send_key_locked(TV_KEY_PLAY_PAUSE);
    if (strcmp(cmd, "TV_PAUSE") == 0)          return tv_send_key_locked(TV_KEY_MEDIA_PAUSE);
    if (strcmp(cmd, "TV_PLAY") == 0)           return tv_send_key_locked(TV_KEY_MEDIA_PLAY);
    if (strcmp(cmd, "TV_NEXT") == 0)           return tv_send_key_locked(TV_KEY_MEDIA_NEXT);
    if (strcmp(cmd, "TV_PREV") == 0)           return tv_send_key_locked(TV_KEY_MEDIA_PREV);
    if (strcmp(cmd, "TV_FWD") == 0)            return tv_send_key_locked(TV_KEY_MEDIA_FWD);
    if (strcmp(cmd, "TV_REW") == 0)            return tv_send_key_locked(TV_KEY_MEDIA_REW);
    if (strcmp(cmd, "TV_VOL_UP") == 0)         return tv_send_key_locked(TV_KEY_VOL_UP, 2);
    if (strcmp(cmd, "TV_VOL_UP_1") == 0)       return tv_send_key_locked(TV_KEY_VOL_UP, 1);
    if (strcmp(cmd, "TV_VOL_DOWN") == 0)       return tv_send_key_locked(TV_KEY_VOL_DOWN, 2);
    if (strcmp(cmd, "TV_VOL_DOWN_1") == 0)     return tv_send_key_locked(TV_KEY_VOL_DOWN, 1);
    if (strcmp(cmd, "TV_MUTE") == 0)           return tv_send_key_locked(TV_KEY_MUTE);
    if (strcmp(cmd, "TV_CH_UP") == 0)          return tv_send_key_locked(TV_KEY_CH_UP);
    if (strcmp(cmd, "TV_CH_DOWN") == 0)        return tv_send_key_locked(TV_KEY_CH_DOWN);
    if (strcmp(cmd, "TV_UP") == 0)             return tv_send_key_locked(TV_KEY_DPAD_UP);
    if (strcmp(cmd, "TV_DOWN") == 0)           return tv_send_key_locked(TV_KEY_DPAD_DOWN);
    if (strcmp(cmd, "TV_LEFT") == 0)           return tv_send_key_locked(TV_KEY_DPAD_LEFT);
    if (strcmp(cmd, "TV_RIGHT") == 0)          return tv_send_key_locked(TV_KEY_DPAD_RIGHT);
    if (strcmp(cmd, "TV_OK") == 0)             return tv_send_key_locked(TV_KEY_DPAD_CENTER);
    if (strcmp(cmd, "TV_BACK") == 0)           return tv_send_key_locked(TV_KEY_BACK);
    if (strcmp(cmd, "TV_HOME") == 0)           return tv_send_key_locked(TV_KEY_HOME);
    if (strcmp(cmd, "TV_MENU") == 0)           return tv_send_key_locked(TV_KEY_MENU);

    if (strcmp(cmd, "TV_OPEN_YOUTUBE") == 0)   return tv_send_app_link_locked("https://www.youtube.com");
    if (strcmp(cmd, "TV_OPEN_NETFLIX") == 0)   return tv_send_app_link_locked("https://www.netflix.com/title");
    if (strcmp(cmd, "TV_OPEN_SPOTIFY") == 0)   return tv_send_app_link_locked("https://open.spotify.com");
    if (strcmp(cmd, "TV_OPEN_PRIME") == 0)     return tv_send_app_link_locked("https://app.primevideo.com");
    if (strcmp(cmd, "TV_OPEN_DISNEY") == 0)    return tv_send_app_link_locked("https://www.disneyplus.com");
    if (strncmp(cmd, "TV_OPEN_URL:", 12) == 0) return tv_send_app_link_locked(cmd + 12);

    if (strncmp(cmd, "TV_YOUTUBE_PLAY:", 16) == 0) {
        char url[220] = {0};
        if (tv_resolve_youtube_via_pc(cmd + 16, url, sizeof(url))) {
            return tv_send_app_link_locked(url);
        }
        return tv_send_app_link_locked("https://www.youtube.com");
    }
    return false;
}

// Drain any queued TV ping messages without blocking
static void tv_service_pings_locked(void) {
    if (!s_tv || !s_tv->connected) return;
    if (!wifi_manager_is_connected() || wifi_manager_is_ap_active()) {
        tv_close_locked();
        return;
    }
    int64_t idle_ms = (esp_timer_get_time() - s_tv->last_activity_us) / 1000;
    if (idle_ms > TV_IDLE_CLOSE_MS) {
        ESP_LOGI(TAG, "TV session idle > %ds -> closing to save battery", TV_IDLE_CLOSE_MS / 1000);
        tv_close_locked();
        return;
    }

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(s_tv->net.fd, &rfds);
    struct timeval tv = { .tv_sec = 0, .tv_usec = 10000 };
    if (mbedtls_ssl_get_bytes_avail(&s_tv->ssl) > 0 || select(s_tv->net.fd + 1, &rfds, nullptr, nullptr, &tv) > 0) {
        uint8_t rx[256];
        int rlen = tv_recv_framed(s_tv, rx, sizeof(rx), 200);
        if (rlen < 0) {
            tv_close_locked();
        } else if (rlen > 0) {
            tv_inspect_and_handle_rx(s_tv, rx, (size_t)rlen);
        }
    }
}

bool tv_control_send(const char* cmd) {
    if (!cmd || strlen(cmd) == 0) return false;
    ensure_tv_mutex();

    // Ensure Wi-Fi is up before taking TV mutex
    if (!wifi_manager_is_connected() && !wifi_manager_is_ap_active()) {
        if (!wifi_manager_is_active()) wifi_manager_start();
        for (int i = 0; i < 35 && !wifi_manager_is_connected(); i++) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
    if (!wifi_manager_is_connected() || wifi_manager_is_ap_active()) return false;

    wifi_manager_mark_activity();
    if (xSemaphoreTake(s_tv_mutex, pdMS_TO_TICKS(6000)) != pdTRUE) return false;

    bool ok = false;
    for (int attempt = 0; attempt < 2 && !ok; attempt++) {
        if (!tv_connect_locked()) continue;
        ok = tv_dispatch_locked(cmd);
        if (!ok) {
            ESP_LOGW(TAG, "TV send [%s] failed on attempt %d -> reconnecting", cmd, attempt + 1);
            tv_close_locked();
        }
    }

    xSemaphoreGive(s_tv_mutex);
    if (ok) {
        ESP_LOGI(TAG, "Direct Watch -> TV [%s] OK!", cmd);
    }
    return ok;
}

void tv_control_disconnect(void) {
    ensure_tv_mutex();
    if (xSemaphoreTake(s_tv_mutex, pdMS_TO_TICKS(1500)) == pdTRUE) {
        tv_close_locked();
        xSemaphoreGive(s_tv_mutex);
    }
}

static void tv_async_worker(void* arg) {
    TvAsyncCmd item;
    while (true) {
        // Wake immediately on command, or check pings/idle every 250ms while connected
        TickType_t wait_ticks = (s_tv && s_tv->connected) ? pdMS_TO_TICKS(250) : portMAX_DELAY;
        if (xQueueReceive(s_tv_queue, &item, wait_ticks) == pdTRUE) {
            tv_control_send(item.cmd);
        } else if (s_tv && s_tv->connected) {
            ensure_tv_mutex();
            if (xSemaphoreTake(s_tv_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                tv_service_pings_locked();
                xSemaphoreGive(s_tv_mutex);
            }
        }
    }
}

void tv_control_send_async(const char* cmd) {
    if (!cmd || strlen(cmd) == 0) return;
    if (!s_tv_queue) {
        s_tv_queue = xQueueCreate(12, sizeof(TvAsyncCmd));
    }
    if (!s_tv_task && s_tv_queue) {
        // 8KB Internal SRAM stack (TLS buffers live in Octal PSRAM!)
        xTaskCreatePinnedToCoreWithCaps(
            tv_async_worker,
            "tv_ctrl_wrk",
            8192,
            nullptr,
            4,
            &s_tv_task,
            1,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
        );
    }
    if (s_tv_queue) {
        TvAsyncCmd item = {};
        strncpy(item.cmd, cmd, sizeof(item.cmd) - 1);
        xQueueSend(s_tv_queue, &item, 0);
    }
}
