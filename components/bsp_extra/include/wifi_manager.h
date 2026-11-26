#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Pre-initialize NVS and timezone only (NO WiFi hardware started)
esp_err_t wifi_manager_pre_init(void);

// Full WiFi start (call only when needed, e.g. from Settings)
esp_err_t wifi_manager_start(void);

// Full WiFi stop and cleanup (frees all WiFi memory)
esp_err_t wifi_manager_stop(void);

// Check if WiFi hardware is currently active
bool wifi_manager_is_active(void);

// Check if WiFi is connected and has an IP address
bool wifi_manager_is_connected(void);

// Start scanning for Wi-Fi networks (non-blocking)
esp_err_t wifi_manager_scan(void);

#define DEFAULT_WIFI_SSID     "YOUR_WIFI_SSID"
#define DEFAULT_WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// Connect to default preconfigured network
esp_err_t wifi_manager_connect_default(void);

// Connect to a Wi-Fi network
esp_err_t wifi_manager_connect(const char* ssid, const char* password);

// Get current connected or connecting SSID
void wifi_manager_get_ssid(char* buf);

// Get the latest scanned networks.
int wifi_manager_get_scanned_networks(char ssids[][33], int max_networks);

// Get the current IP address as string (e.g. "192.168.1.100")
void wifi_manager_get_ip(char* buf);

// Get the count of discovered APs from last scan
int wifi_manager_get_ap_count(void);

typedef enum {
    WIFI_STATE_OFF,
    WIFI_STATE_INIT,
    WIFI_STATE_IDLE,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_CONNECTED,
    WIFI_STATE_GOT_IP,
    WIFI_STATE_FAILED
} wifi_state_t;

// Get the current Wi-Fi state
wifi_state_t wifi_manager_get_state(void);

// Start Hotspot / SoftAP mode for direct offline connection without any router
esp_err_t wifi_manager_start_ap(const char* ssid, const char* password);

// Check if SoftAP mode is currently active
bool wifi_manager_is_ap_active(void);

// Non-blocking async wrappers (safe to call from LVGL UI callbacks without freezing UI)
void wifi_manager_start_async(void);
void wifi_manager_stop_async(void);
void wifi_manager_connect_default_async(void);

// Thread-safe modem-sleep refresh for STA mode (safe to call from display wake callback)
void wifi_manager_refresh_ps(void);

// Temporarily disable Wi-Fi modem-sleep (WIFI_PS_NONE) during short low-latency API bursts
void wifi_manager_set_turbo(bool enable);

// Smart 90s Idle Auto-Off hooks (keeps Wi-Fi hot between consecutive commands, shuts off 0mA when idle in sleep!)
void wifi_manager_mark_activity(void);
void wifi_manager_on_screen_sleep(void);
void wifi_manager_on_screen_wake(void);

#ifdef __cplusplus
}
#endif
