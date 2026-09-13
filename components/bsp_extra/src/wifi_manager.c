#include "wifi_manager.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "rtc_lib.h"
#include <string.h>

static const char *TAG = "WifiManager";

#define MAX_SCAN_NETWORKS 15
static wifi_ap_record_t ap_info[MAX_SCAN_NETWORKS];
static uint16_t ap_count = 0;
static wifi_state_t current_state = WIFI_STATE_OFF;
static bool wifi_active = false;
static bool nvs_initialized = false;
static bool netif_initialized = false;
static esp_netif_t *sta_netif = NULL;
static esp_event_handler_instance_t instance_any_id = NULL;
static esp_event_handler_instance_t instance_got_ip = NULL;

static void wifi_print_telemetry(const char* state_name) {
    ESP_LOGI("MEM", "--- WIFI STATE: %s ---", state_name);
    ESP_LOGI("MEM", "INTERNAL free=%u largest=%u DMA free=%u largest=%u PSRAM free=%u largest=%u",
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        heap_caps_get_free_size(MALLOC_CAP_DMA), heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM), heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

wifi_state_t wifi_manager_get_state(void)
{
    return current_state;
}

static void time_sync_notification_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "SNTP time sync event received");
    
    struct tm timeinfo;
    time_t now = tv->tv_sec;
    localtime_r(&now, &timeinfo);
    
    rtc_set_time(&timeinfo);
    ESP_LOGI(TAG, "Hardware RTC updated: %04d-%02d-%02d %02d:%02d:%02d",
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                    int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Wi-Fi started (STA_START)");
        current_state = WIFI_STATE_IDLE;
        // Start non-blocking network scan immediately
        wifi_manager_scan();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "Wi-Fi disconnected");
        if (current_state == WIFI_STATE_CONNECTING) {
            static int s_retry_count = 0;
            if (s_retry_count < 3) {
                s_retry_count++;
                ESP_LOGI(TAG, "Reconnecting attempt %d...", s_retry_count);
                esp_wifi_connect();
            } else {
                s_retry_count = 0;
                current_state = WIFI_STATE_IDLE;
                ESP_LOGW(TAG, "Connection attempts exhausted, returned to IDLE");
            }
        } else {
            current_state = WIFI_STATE_IDLE;
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG, "[WIFI] STA_CONNECTED");
        current_state = WIFI_STATE_CONNECTED;
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "[IP] GOT_IP: " IPSTR, IP2STR(&event->ip_info.ip));
        current_state = WIFI_STATE_GOT_IP;
        wifi_print_telemetry("GOT_IP");

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
        uint16_t number = MAX_SCAN_NETWORKS;
        esp_wifi_scan_get_ap_records(&number, ap_info);
        ap_count = number;
        ESP_LOGI(TAG, "Scan completed, found %d networks", ap_count);
    }
}

// Pre-init: only NVS + timezone (called at boot, NO WiFi hardware)
esp_err_t wifi_manager_pre_init(void)
{
    if (!nvs_initialized) {
        esp_err_t ret = nvs_flash_init();
        if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_ERROR_CHECK(nvs_flash_erase());
            ret = nvs_flash_init();
        }
        ESP_ERROR_CHECK(ret);
        nvs_initialized = true;
    }

    // Set timezone (Turkey UTC+3)
    setenv("TZ", "TRT-3", 1);
    tzset();

    ESP_LOGI(TAG, "Pre-init done (NVS + TZ only, no WiFi hardware)");
    return ESP_OK;
}

// Full WiFi start — called on-demand from Settings UI
esp_err_t wifi_manager_start(void)
{
    if (wifi_active) {
        ESP_LOGW(TAG, "WiFi already active");
        return ESP_OK;
    }
    
    current_state = WIFI_STATE_INIT;
    wifi_print_telemetry("INIT");

    // Ensure NVS is ready
    if (!nvs_initialized) {
        wifi_manager_pre_init();
    }

    // Initialize netif (only once)
    if (!netif_initialized) {
        ESP_ERROR_CHECK(esp_netif_init());
        
        esp_err_t loop_ret = esp_event_loop_create_default();
        if (loop_ret != ESP_OK && loop_ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "Failed to create default event loop");
            return loop_ret;
        }
        netif_initialized = true;
    }

    if (sta_netif == NULL) {
        sta_netif = esp_netif_create_default_wifi_sta();
    }

    wifi_active = false;
    current_state = WIFI_STATE_OFF;
    ESP_LOGW(TAG, "--- MEMORY BEFORE WIFI STOP ---");
    ESP_LOGW(TAG, "INTERNAL Free: %u, Largest: %u", heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    ESP_LOGW(TAG, "DMA Free: %u", heap_caps_get_free_size(MALLOC_CAP_DMA));
    ESP_LOGW(TAG, "PSRAM Free: %u, Largest: %u", heap_caps_get_free_size(MALLOC_CAP_SPIRAM), heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    // Wearable Ultra-low-memory Wi-Fi profile (saves ~50KB internal SRAM)
    cfg.static_rx_buf_num = 4;
    cfg.dynamic_rx_buf_num = 8;
    cfg.tx_buf_type = 1; // Dynamic TX buffers
    cfg.static_tx_buf_num = 0;
    cfg.dynamic_tx_buf_num = 8;
    cfg.cache_tx_buf_num = 4;
    cfg.mgmt_sbuf_num = 6;
    cfg.ampdu_rx_enable = 0;
    cfg.ampdu_tx_enable = 0;
    cfg.rx_ba_win = 4;

    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(err));
        current_state = WIFI_STATE_FAILED;
        return err;
    }

    err = esp_event_handler_instance_register(WIFI_EVENT,
                                              ESP_EVENT_ANY_ID,
                                              &wifi_event_handler,
                                              NULL,
                                              &instance_any_id);
    if (err != ESP_OK) return err;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_active = true;
    current_state = WIFI_STATE_IDLE;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    // 1. Cap Max TX power to 13 dBm (52 in 0.25 dBm units) to eliminate brownouts & white screen
    esp_wifi_set_max_tx_power(52);

    // 2. Enable Modem-Sleep power saving (radio sleeps between beacons)
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));

    ESP_LOGI(TAG, "WiFi hardware started (Safe TX Power 13dBm + Modem Sleep)");
    return ESP_OK;
}

esp_err_t wifi_manager_stop(void)
{
    if (!wifi_active) {
        ESP_LOGW(TAG, "WiFi not active");
        return ESP_OK;
    }

    if (esp_sntp_enabled()) {
        esp_sntp_stop();
    }

    esp_wifi_scan_stop();
    esp_wifi_disconnect();
    esp_wifi_stop();

    if (instance_any_id) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id);
        instance_any_id = NULL;
    }
    if (instance_got_ip) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip);
        instance_got_ip = NULL;
    }

    esp_wifi_deinit();

    wifi_active = false;
    current_state = WIFI_STATE_OFF;
    ap_count = 0;
    ESP_LOGI(TAG, "WiFi hardware stopped and memory freed");
    return ESP_OK;
}

bool wifi_manager_is_active(void)
{
    return wifi_active;
}

int wifi_manager_get_ap_count(void)
{
    return ap_count;
}

esp_err_t wifi_manager_scan(void)
{
    if (!wifi_active) return ESP_ERR_WIFI_NOT_STARTED;
    
    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = {
            .active = {
                .min = 100,
                .max = 200
            }
        }
    };
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, false);
    ESP_LOGI(TAG, "wifi_manager_scan non-blocking start -> %s", esp_err_to_name(err));
    return err;
}

int wifi_manager_get_scanned_networks(char ssids[][33], int max_networks)
{
    int count = 0;
    for (int i = 0; i < ap_count && count < max_networks; i++) {
        if (strlen((char *)ap_info[i].ssid) > 0) {
            strncpy(ssids[count], (char *)ap_info[i].ssid, 32);
            ssids[count][32] = '\0';
            count++;
        }
    }
    return count;
}

typedef struct {
    char ssid[33];
    char password[65];
} conn_args_t;

static void wifi_connect_task_func(void* pvParam) {
    conn_args_t* a = (conn_args_t*)pvParam;
    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, a->ssid, sizeof(wifi_config.sta.ssid));
    if (strlen(a->password) > 0) {
        strncpy((char *)wifi_config.sta.password, a->password, sizeof(wifi_config.sta.password));
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    ESP_LOGI(TAG, "esp_wifi_set_config -> %s", esp_err_to_name(err));
    if (err == ESP_OK) {
        err = esp_wifi_connect();
        ESP_LOGI(TAG, "esp_wifi_connect -> %s", esp_err_to_name(err));
    }
    free(a);
    vTaskDelete(NULL);
}

esp_err_t wifi_manager_connect(const char* ssid, const char* password)
{
    if (!wifi_active) return ESP_ERR_WIFI_NOT_STARTED;
    
    current_state = WIFI_STATE_CONNECTING;
    wifi_print_telemetry("START / CONNECTING");
    
    conn_args_t* args = (conn_args_t*)malloc(sizeof(conn_args_t));
    if (!args) return ESP_ERR_NO_MEM;
    strncpy(args->ssid, ssid, sizeof(args->ssid)-1);
    args->ssid[sizeof(args->ssid)-1] = '\0';
    if (password) {
        strncpy(args->password, password, sizeof(args->password)-1);
        args->password[sizeof(args->password)-1] = '\0';
    } else {
        args->password[0] = '\0';
    }

    xTaskCreate(wifi_connect_task_func, "wifi_connect", 4096, args, 5, NULL);

    return ESP_OK;
}

bool wifi_manager_is_connected(void)
{
    return (current_state == WIFI_STATE_GOT_IP);
}

void wifi_manager_get_ip(char* buf)
{
    if (buf == NULL) return;
    buf[0] = '\0';
    if (current_state == WIFI_STATE_GOT_IP && sta_netif != NULL) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK) {
            sprintf(buf, IPSTR, IP2STR(&ip_info.ip));
        }
    }
}
