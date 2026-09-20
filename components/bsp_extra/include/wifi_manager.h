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

#define DEFAULT_WIFI_SSID     "TurkTelekom_ZYA41B"
#define DEFAULT_WIFI_PASSWORD "cgXy77WVzfYF"

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

#ifdef __cplusplus
}
#endif
