#include "pc_control.hpp"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

extern "C" {
#include "wifi_manager.h"
#include "ble_manager.h"
}

static const char* TAG = "PC_CTRL";

#define PC_DEFAULT_IP     "192.168.1.193"
#define PC_HTTP_PORT      5000
#define PC_DISCOVER_PORT  5001
#define ESPHOME_SALON_IP  "192.168.1.102"

// DESKTOP-948UKTL Ethernet MAC: 50:EB:F6:B9:A5:83
static const uint8_t s_pc_mac[6] = { 0x50, 0xEB, 0xF6, 0xB9, 0xA5, 0x83 };
static char s_cached_pc_ip[32] = PC_DEFAULT_IP;

struct PcAsyncCmd {
    char cmd[256];
};

static QueueHandle_t s_pc_queue = nullptr;
static TaskHandle_t s_pc_task = nullptr;

const char* pc_control_get_cached_ip(void) {
    return s_cached_pc_ip;
}

bool pc_control_esphome(const char* path) {
    if (!path) return false;
    if (!wifi_manager_is_connected()) {
        if (ble_manager_is_connected()) {
            ble_manager_send_media_command("SMART_HOME|LIGHT_1_TOGGLE");
            return true;
        }
        return false;
    }

    wifi_manager_mark_activity();
    struct sockaddr_in dest_addr = {};
    dest_addr.sin_addr.s_addr = inet_addr(ESPHOME_SALON_IP);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(80);

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) return false;

    struct timeval tv = { .tv_sec = 1, .tv_usec = 200000 };
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    bool ok = false;
    if (connect(sock, (struct sockaddr*)&dest_addr, sizeof(dest_addr)) == 0) {
        char req[192];
        snprintf(req, sizeof(req),
                 "POST %s HTTP/1.1\r\nHost: %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
                 path, ESPHOME_SALON_IP);
        if (send(sock, req, strlen(req), 0) > 0) {
            char rx[64] = {0};
            recv(sock, rx, sizeof(rx) - 1, 0);
            ok = true;
            ESP_LOGI(TAG, "ESPHome %s -> OK", path);
        }
    } else {
        ESP_LOGW(TAG, "ESPHome %s connect failed -> BLE fallback", path);
        if (ble_manager_is_connected()) {
            ble_manager_send_media_command("SMART_HOME|LIGHT_1_TOGGLE");
        }
    }
    close(sock);
    return ok;
}

bool pc_control_wake_lan(void) {
    if (!wifi_manager_is_connected()) {
        if (ble_manager_is_connected()) {
            ble_manager_send_media_command("SMART_HOME|PC_WAKE");
            return true;
        }
        return false;
    }

    wifi_manager_mark_activity();
    // Build standard 102-byte Wake-on-LAN Magic Packet: 6x 0xFF + 16x MAC
    uint8_t magic[102];
    memset(magic, 0xFF, 6);
    for (int i = 0; i < 16; i++) {
        memcpy(magic + 6 + (i * 6), s_pc_mac, 6);
    }

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return false;

    int broadcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

    const char* targets[3] = { "192.168.1.255", "255.255.255.255", s_cached_pc_ip };
    const uint16_t ports[2] = { 9, 7 };
    bool sent_any = false;
    for (int r = 0; r < 3; r++) {
        for (int t = 0; t < 3; t++) {
            for (int p = 0; p < 2; p++) {
                if (t == 2 && p == 1) continue; // Unicast port 9 is enough
                struct sockaddr_in dst = {};
                dst.sin_family = AF_INET;
                dst.sin_port = htons(ports[p]);
                dst.sin_addr.s_addr = inet_addr(targets[t]);
                if (sendto(sock, magic, sizeof(magic), 0, (struct sockaddr*)&dst, sizeof(dst)) == (ssize_t)sizeof(magic)) {
                    sent_any = true;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(35));
    }
    close(sock);

    ESP_LOGI(TAG, "Direct Watch UDP Wake-on-LAN (3x bursts, ports 9+7, bcast+unicast) sent to 50:EB:F6:B9:A5:83 (ok=%d)", (int)sent_any);
    return sent_any;
}

static bool pc_discover_ip_udp(void) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return false;

    int broadcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
    struct timeval tv = { .tv_sec = 0, .tv_usec = 450000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in dst = {};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(PC_DISCOVER_PORT);
    dst.sin_addr.s_addr = inet_addr("192.168.1.255");

    const char* probe = "UZ_PC_DISCOVER";
    sendto(sock, probe, strlen(probe), 0, (struct sockaddr*)&dst, sizeof(dst));

    char rx[96] = {0};
    struct sockaddr_in src = {};
    socklen_t srclen = sizeof(src);
    int n = recvfrom(sock, rx, sizeof(rx) - 1, 0, (struct sockaddr*)&src, &srclen);
    close(sock);

    if (n > 11 && strncmp(rx, "UZ_PC_HERE|", 11) == 0) {
        char* p = rx + 11;
        char* sep = strchr(p, '|');
        if (sep) *sep = '\0';
        if (strlen(p) >= 7 && strlen(p) < sizeof(s_cached_pc_ip)) {
            strncpy(s_cached_pc_ip, p, sizeof(s_cached_pc_ip) - 1);
            s_cached_pc_ip[sizeof(s_cached_pc_ip) - 1] = '\0';
            ESP_LOGI(TAG, "Auto-discovered PC Agent IP: %s", s_cached_pc_ip);
            return true;
        }
    }
    return false;
}

static bool pc_http_post_cmd(const char* ip, const char* cmd) {
    struct sockaddr_in dst = {};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(PC_HTTP_PORT);
    dst.sin_addr.s_addr = inet_addr(ip);

    char* req = (char*)heap_caps_malloc(640, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char* rx = (char*)heap_caps_malloc(192, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!req) req = (char*)malloc(640);
    if (!rx) rx = (char*)malloc(192);
    if (!req || !rx) {
        free(req);
        free(rx);
        return false;
    }

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        free(req);
        free(rx);
        return false;
    }

    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    bool ok = false;
    if (connect(sock, (struct sockaddr*)&dst, sizeof(dst)) == 0) {
        int req_len = snprintf(req, 640,
                               "POST /command HTTP/1.1\r\n"
                               "Host: %s:%d\r\n"
                               "Content-Type: text/plain; charset=utf-8\r\n"
                               "Content-Length: %u\r\n"
                               "Connection: close\r\n\r\n%s",
                               ip, PC_HTTP_PORT, (unsigned)strlen(cmd), cmd);
        if (req_len > 0 && req_len < 640 && send(sock, req, req_len, 0) > 0) {
            memset(rx, 0, 192);
            int r = recv(sock, rx, 191, 0);
            if (r > 0 && strstr(rx, "200") != nullptr) {
                ok = true;
            }
        }
    }
    close(sock);
    free(req);
    free(rx);
    return ok;
}

static bool pc_http_get_stats_from_ip(const char* ip, char* out_summary, size_t max_len) {
    struct sockaddr_in dst = {};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(PC_HTTP_PORT);
    dst.sin_addr.s_addr = inet_addr(ip);

    char* req = (char*)heap_caps_malloc(160, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char* rx = (char*)heap_caps_malloc(768, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!req) req = (char*)malloc(160);
    if (!rx) rx = (char*)malloc(768);
    if (!req || !rx) {
        free(req);
        free(rx);
        return false;
    }

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        free(req);
        free(rx);
        return false;
    }

    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    bool ok = false;
    if (connect(sock, (struct sockaddr*)&dst, sizeof(dst)) == 0) {
        int req_len = snprintf(req, 160,
                               "GET /stats HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\n\r\n",
                               ip, PC_HTTP_PORT);
        if (req_len > 0 && send(sock, req, req_len, 0) > 0) {
            memset(rx, 0, 768);
            size_t total = 0;
            while (total + 1 < 768) {
                int r = recv(sock, rx + total, 767 - total, 0);
                if (r <= 0) break;
                total += (size_t)r;
            }
            rx[total] = '\0';
            const char* key = strstr(rx, "\"summary\"");
            if (key) {
                const char* colon = strchr(key + 9, ':');
                const char* q1 = colon ? strchr(colon, '"') : nullptr;
                const char* q2 = q1 ? strchr(q1 + 1, '"') : nullptr;
                if (q1 && q2 && q2 > q1 + 1) {
                    size_t slen = (size_t)(q2 - (q1 + 1));
                    if (slen >= max_len) slen = max_len - 1;
                    memcpy(out_summary, q1 + 1, slen);
                    out_summary[slen] = '\0';
                    ok = true;
                }
            }
        }
    }
    close(sock);
    free(req);
    free(rx);
    return ok;
}

bool pc_control_get_stats(char* out_summary, size_t max_len) {
    if (!out_summary || max_len < 16) return false;
    out_summary[0] = '\0';
    if (!wifi_manager_is_connected()) return false;
    wifi_manager_mark_activity();
    if (pc_http_get_stats_from_ip(s_cached_pc_ip, out_summary, max_len)) {
        ESP_LOGI(TAG, "PC Stats @ %s: %s", s_cached_pc_ip, out_summary);
        return true;
    }
    if (pc_discover_ip_udp() && pc_http_get_stats_from_ip(s_cached_pc_ip, out_summary, max_len)) {
        ESP_LOGI(TAG, "PC Stats @ %s (auto-discovered): %s", s_cached_pc_ip, out_summary);
        return true;
    }
    return false;
}

bool pc_control_send(const char* cmd) {
    if (!cmd || strlen(cmd) == 0) return false;

    if (strcmp(cmd, "PC_WAKE") == 0) {
        return pc_control_wake_lan();
    }

    if (wifi_manager_is_connected()) {
        wifi_manager_mark_activity();
        if (pc_http_post_cmd(s_cached_pc_ip, cmd)) {
            ESP_LOGI(TAG, "Direct Watch -> PC [%s] @ %s OK!", cmd, s_cached_pc_ip);
            return true;
        }
        // Retry once via UDP auto-discovery in case PC DHCP IP changed
        if (pc_discover_ip_udp() && pc_http_post_cmd(s_cached_pc_ip, cmd)) {
            ESP_LOGI(TAG, "Direct Watch -> PC [%s] @ %s (auto-discovered) OK!", cmd, s_cached_pc_ip);
            return true;
        }
        ESP_LOGW(TAG, "Direct Watch -> PC [%s] unreachable on %s:%d", cmd, s_cached_pc_ip, PC_HTTP_PORT);
    }

    // Fallback via BLE to Android Companion if connected
    if (ble_manager_is_connected()) {
        char ble_buf[160];
        if (strncmp(cmd, "SMART_HOME|", 11) == 0) {
            snprintf(ble_buf, sizeof(ble_buf), "%s", cmd);
        } else {
            snprintf(ble_buf, sizeof(ble_buf), "SMART_HOME|%s", cmd);
        }
        ble_manager_send_media_command(ble_buf);
        return true;
    }
    return false;
}

static void pc_async_worker(void* arg) {
    PcAsyncCmd item;
    while (true) {
        if (xQueueReceive(s_pc_queue, &item, portMAX_DELAY) == pdTRUE) {
            // Special composite modes executed directly from UI buttons
            if (strcmp(item.cmd, "COMBO_BREAK") == 0) {
                if (!wifi_manager_is_connected() && !wifi_manager_is_ap_active()) {
                    if (!wifi_manager_is_active()) wifi_manager_start();
                    for (int i = 0; i < 35 && !wifi_manager_is_connected(); i++) vTaskDelay(pdMS_TO_TICKS(200));
                }
                pc_control_send("PC_MODE_BREAK");
                pc_control_esphome("/switch/salon_isigi/turn_on");
                continue;
            }
            if (strcmp(item.cmd, "COMBO_MOVIE") == 0) {
                if (!wifi_manager_is_connected() && !wifi_manager_is_ap_active()) {
                    if (!wifi_manager_is_active()) wifi_manager_start();
                    for (int i = 0; i < 35 && !wifi_manager_is_connected(); i++) vTaskDelay(pdMS_TO_TICKS(200));
                }
                pc_control_send("PC_MODE_MOVIE");
                pc_control_esphome("/switch/salon_isigi/turn_off");
                continue;
            }
            if (strcmp(item.cmd, "LIGHT_TOGGLE") == 0) {
                if (!wifi_manager_is_connected() && !wifi_manager_is_ap_active()) {
                    if (!wifi_manager_is_active()) wifi_manager_start();
                    for (int i = 0; i < 35 && !wifi_manager_is_connected(); i++) vTaskDelay(pdMS_TO_TICKS(200));
                }
                pc_control_esphome("/switch/salon_isigi/toggle");
                continue;
            }

            // Standard PC command: ALWAYS bring up Watch Wi-Fi first for direct LAN control!
            // (Android Companion BLE does not execute PC_* commands; BLE is only a fallback if Wi-Fi fails.)
            if (!wifi_manager_is_connected() && !wifi_manager_is_ap_active()) {
                if (!wifi_manager_is_active()) wifi_manager_start();
                for (int i = 0; i < 35 && !wifi_manager_is_connected(); i++) vTaskDelay(pdMS_TO_TICKS(200));
            }
            pc_control_send(item.cmd);
        }
    }
}

void pc_control_send_async(const char* cmd) {
    if (!cmd || strlen(cmd) == 0) return;
    if (s_pc_queue == nullptr) {
        s_pc_queue = xQueueCreate(8, sizeof(PcAsyncCmd));
    }
    if (s_pc_task == nullptr && s_pc_queue != nullptr) {
        xTaskCreatePinnedToCoreWithCaps(
            pc_async_worker,
            "pc_ctrl_wrk",
            6144,
            nullptr,
            4,
            &s_pc_task,
            1,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
        );
    }
    if (s_pc_queue != nullptr) {
        PcAsyncCmd item = {};
        strncpy(item.cmd, cmd, sizeof(item.cmd) - 1);
        xQueueSend(s_pc_queue, &item, 0);
    }
}
