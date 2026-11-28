#include "wifi_manager.h"
#include "ble_manager.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "rtc_lib.h"
#include <string.h>
#include <stdint.h>

static const char *TAG = "WifiManager";

#define MAX_SCAN_NETWORKS 15
static wifi_ap_record_t ap_info[MAX_SCAN_NETWORKS];
static uint16_t ap_count = 0;
static volatile wifi_state_t current_state = WIFI_STATE_OFF;
static volatile bool wifi_active = false;
static bool nvs_initialized = false;
static bool netif_initialized = false;
static esp_netif_t *sta_netif = NULL;
static esp_netif_t *ap_netif = NULL;
static volatile bool ap_active = false;
static esp_event_handler_instance_t instance_any_id = NULL;
static esp_event_handler_instance_t instance_got_ip = NULL;
static int s_retry_count = 0;
static volatile uint32_t s_wifi_session_id = 0;
static SemaphoreHandle_t s_wifi_mutex = NULL;

typedef enum {
    WIFI_ASYNC_START = 1,
    WIFI_ASYNC_STOP = 2,
    WIFI_ASYNC_CONNECT_DEFAULT = 3,
} wifi_async_cmd_t;

static QueueHandle_t s_wifi_async_queue = NULL;
static TaskHandle_t s_wifi_async_task = NULL;
static esp_timer_handle_t s_idle_off_timer = NULL;
static volatile bool s_screen_sleeping = false;
static volatile int64_t s_last_activity_us = 0;

#define WIFI_IDLE_SLEEP_TIMEOUT_US (90ULL * 1000000ULL) // 90 seconds after screen sleep -> auto-stop STA (0 mA!)

static void wifi_idle_off_timer_cb(void* arg) {
    if (!wifi_active || ap_active || !s_screen_sleeping) return;
    int64_t idle_us = esp_timer_get_time() - s_last_activity_us;
    if (idle_us >= (int64_t)WIFI_IDLE_SLEEP_TIMEOUT_US - 2000000LL) {
        ESP_LOGI(TAG, "Smart Power: Wi-Fi STA idle 90s while screen sleeping -> auto-stopping radio (0 mA)!");
        wifi_manager_stop_async();
    } else {
        uint64_t rem_us = WIFI_IDLE_SLEEP_TIMEOUT_US - (uint64_t)idle_us;
        (void)esp_timer_start_once(s_idle_off_timer, rem_us);
    }
}

static void ensure_idle_timer(void) {
    if (s_idle_off_timer == NULL) {
        esp_timer_create_args_t args = {
            .callback = wifi_idle_off_timer_cb,
            .arg = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "wifi_idle_off",
            .skip_unhandled_events = true,
        };
        (void)esp_timer_create(&args, &s_idle_off_timer);
    }
}

void wifi_manager_mark_activity(void) {
    s_last_activity_us = esp_timer_get_time();
    if (s_idle_off_timer && s_screen_sleeping && wifi_active && !ap_active) {
        esp_timer_stop(s_idle_off_timer);
        (void)esp_timer_start_once(s_idle_off_timer, WIFI_IDLE_SLEEP_TIMEOUT_US);
    }
}

void wifi_manager_on_screen_sleep(void) {
    s_screen_sleeping = true;
    s_last_activity_us = esp_timer_get_time();
    if (wifi_active && !ap_active) {
        ensure_idle_timer();
        if (s_idle_off_timer) {
            esp_timer_stop(s_idle_off_timer);
            (void)esp_timer_start_once(s_idle_off_timer, WIFI_IDLE_SLEEP_TIMEOUT_US);
            ESP_LOGI(TAG, "Smart Power: Wi-Fi STA will stay warm for 90s then auto-stop if unused.");
        }
    }
}

void wifi_manager_on_screen_wake(void) {
    s_screen_sleeping = false;
    s_last_activity_us = esp_timer_get_time();
    if (s_idle_off_timer) {
        esp_timer_stop(s_idle_off_timer);
    }
    wifi_manager_refresh_ps();
}

static void ensure_wifi_mutex(void) {
    if (s_wifi_mutex == NULL) {
        s_wifi_mutex = xSemaphoreCreateRecursiveMutex();
    }
}

static void wifi_print_telemetry(const char* state_name) {
    ESP_LOGI("MEM", "--- WIFI STATE: %s ---", state_name);
    ESP_LOGI("MEM", "INTERNAL free=%u largest=%u DMA free=%u largest=%u PSRAM free=%u largest=%u",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
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

static void trigger_wifi_auto_stop(uint32_t session_id) {
    (void)session_id;
    if (ap_active) return;
    // Reuse existing wifi_async worker instead of spawning a new 4KB task from sys_evt!
    wifi_manager_stop_async();
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (ap_active || !wifi_active) return;
        ESP_LOGI(TAG, "Wi-Fi started (STA_START) -> Auto-connecting to %s...", DEFAULT_WIFI_SSID);
        current_state = WIFI_STATE_CONNECTING;
        s_retry_count = 0;
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (ap_active || !wifi_active) return;
        wifi_event_sta_disconnected_t* disc = (wifi_event_sta_disconnected_t*)event_data;
        ESP_LOGW(TAG, "Wi-Fi disconnected (reason: %d)", disc ? disc->reason : -1);
        if (s_retry_count < 2) {
            s_retry_count++;
            current_state = WIFI_STATE_CONNECTING;
            ESP_LOGI(TAG, "Reconnecting attempt %d/2...", s_retry_count);
            esp_wifi_connect();
        } else {
            s_retry_count = 0;
            current_state = WIFI_STATE_FAILED;
            ESP_LOGW(TAG, "Wi-Fi AP unreachable after 2 attempts. Auto-stopping Wi-Fi to save battery!");
            trigger_wifi_auto_stop(s_wifi_session_id);
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        if (ap_active || !wifi_active) return;
        ESP_LOGI(TAG, "[WIFI] STA_CONNECTED to %s", DEFAULT_WIFI_SSID);
        current_state = WIFI_STATE_CONNECTED;
        s_retry_count = 0;
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        if (ap_active || !wifi_active) return;
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "[IP] GOT_IP: " IPSTR, IP2STR(&event->ip_info.ip));
        current_state = WIFI_STATE_GOT_IP;
        wifi_print_telemetry("GOT_IP");

        if (!esp_sntp_enabled()) {
            esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, "pool.ntp.org");
            sntp_set_time_sync_notification_cb(time_sync_notification_cb);
            esp_sntp_init();
            ESP_LOGI(TAG, "SNTP time sync initialized");
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
        uint16_t number = MAX_SCAN_NETWORKS;
        if (esp_wifi_scan_get_ap_records(&number, ap_info) == ESP_OK) {
            ap_count = number;
            ESP_LOGI(TAG, "Scan completed, found %d networks", ap_count);
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "Phone connected to Watch SoftAP Hotspot!");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        ESP_LOGI(TAG, "Phone disconnected from Watch SoftAP Hotspot");
    }
}

esp_err_t wifi_manager_pre_init(void)
{
    ensure_wifi_mutex();
    if (!nvs_initialized) {
        esp_err_t ret = nvs_flash_init();
        if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            nvs_flash_erase();
            ret = nvs_flash_init();
        }
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(ret));
            return ret;
        }
        nvs_initialized = true;
    }

    setenv("TZ", "TRT-3", 1);
    tzset();

    if (!netif_initialized) {
        (void)esp_netif_init();
        (void)esp_event_loop_create_default();
        netif_initialized = true;
    }

    ESP_LOGI(TAG, "Pre-init done (NVS + TZ + Netif early init)");
    return ESP_OK;
}

static void wifi_connect_watchdog_task(void* pv) {
    uint32_t my_session = (uint32_t)(uintptr_t)pv;
    for (int i = 0; i < 60; i++) {
        vTaskDelay(pdMS_TO_TICKS(250));
        if (s_wifi_session_id != my_session || !wifi_active || ap_active || current_state == WIFI_STATE_GOT_IP) {
            vTaskDelete(NULL);
            return;
        }
    }
    if (s_wifi_session_id == my_session && wifi_active && !ap_active &&
        current_state != WIFI_STATE_GOT_IP && current_state != WIFI_STATE_CONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi watchdog timeout (15s) -> Shutting down Wi-Fi radio to preserve battery!");
        wifi_manager_stop();
    }
    vTaskDelete(NULL);
}

static esp_err_t ensure_netif_and_loop(void) {
    if (!netif_initialized) {
        esp_err_t err = esp_netif_init();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
            return err;
        }
        esp_err_t loop_ret = esp_event_loop_create_default();
        if (loop_ret != ESP_OK && loop_ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "Failed to create default event loop: %s", esp_err_to_name(loop_ret));
            return loop_ret;
        }
        netif_initialized = true;
    }
    return ESP_OK;
}

esp_err_t wifi_manager_start(void)
{
    ensure_wifi_mutex();
    xSemaphoreTakeRecursive(s_wifi_mutex, portMAX_DELAY);

    if (wifi_active && !ap_active) {
        ESP_LOGW(TAG, "WiFi STA already active");
        xSemaphoreGiveRecursive(s_wifi_mutex);
        return ESP_OK;
    }
    if (wifi_active && ap_active) {
        wifi_manager_stop();
    }

    uint32_t session_id = ++s_wifi_session_id;
    current_state = WIFI_STATE_INIT;
    wifi_print_telemetry("STA_INIT");

    if (!nvs_initialized) {
        wifi_manager_pre_init();
    }

    esp_err_t err = ensure_netif_and_loop();
    if (err != ESP_OK) {
        current_state = WIFI_STATE_FAILED;
        xSemaphoreGiveRecursive(s_wifi_mutex);
        return err;
    }

    if (sta_netif == NULL) {
        sta_netif = esp_netif_create_default_wifi_sta();
    }

    // Keep STATIC TX buffers (tx_buf_type = 0 from sdkconfig) — mandatory with Octal PSRAM!
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = 0;
    cfg.static_rx_buf_num = 6;
    cfg.dynamic_rx_buf_num = 24;
    cfg.static_tx_buf_num = 8;

    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(err));
        current_state = WIFI_STATE_FAILED;
        xSemaphoreGiveRecursive(s_wifi_mutex);
        return err;
    }

    if (instance_any_id == NULL) {
        err = esp_event_handler_instance_register(WIFI_EVENT,
                                                  ESP_EVENT_ANY_ID,
                                                  &wifi_event_handler,
                                                  NULL,
                                                  &instance_any_id);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Event register WIFI_EVENT failed: %s", esp_err_to_name(err));
            esp_wifi_deinit();
            current_state = WIFI_STATE_FAILED;
            xSemaphoreGiveRecursive(s_wifi_mutex);
            return err;
        }
    }
    if (instance_got_ip == NULL) {
        err = esp_event_handler_instance_register(IP_EVENT,
                                                  IP_EVENT_STA_GOT_IP,
                                                  &wifi_event_handler,
                                                  NULL,
                                                  &instance_got_ip);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Event register IP_EVENT failed: %s", esp_err_to_name(err));
            esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id);
            instance_any_id = NULL;
            esp_wifi_deinit();
            current_state = WIFI_STATE_FAILED;
            xSemaphoreGiveRecursive(s_wifi_mutex);
            return err;
        }
    }

    ap_active = false;
    wifi_active = true;
    current_state = WIFI_STATE_IDLE;

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode(STA) failed: %s", esp_err_to_name(err));
        wifi_manager_stop();
        xSemaphoreGiveRecursive(s_wifi_mutex);
        return err;
    }

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, DEFAULT_WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, DEFAULT_WIFI_PASSWORD, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start(STA) failed: %s", esp_err_to_name(err));
        wifi_manager_stop();
        xSemaphoreGiveRecursive(s_wifi_mutex);
        return err;
    }

    // Cap Max TX power to 13 dBm (52 in 0.25 dBm units) to eliminate brownouts
    esp_wifi_set_max_tx_power(52);

    // Enable Modem-Sleep power saving (required for BLE + Wi-Fi STA coexistence)
    (void)esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    s_last_activity_us = esp_timer_get_time();
    if (s_screen_sleeping) {
        ensure_idle_timer();
        if (s_idle_off_timer) {
            esp_timer_stop(s_idle_off_timer);
            (void)esp_timer_start_once(s_idle_off_timer, WIFI_IDLE_SLEEP_TIMEOUT_US);
        }
    }

    xTaskCreate(wifi_connect_watchdog_task, "wifi_wdog", 3072, (void*)(uintptr_t)session_id, 3, NULL);

    ESP_LOGI(TAG, "WiFi STA started & default AP (%s) configured", DEFAULT_WIFI_SSID);
    xSemaphoreGiveRecursive(s_wifi_mutex);
    return ESP_OK;
}

esp_err_t wifi_manager_stop(void)
{
    ensure_wifi_mutex();
    xSemaphoreTakeRecursive(s_wifi_mutex, portMAX_DELAY);

    s_wifi_session_id++; // Cooperatively cancels any running watchdog/autostop without unsafe vTaskDelete!
    if (s_idle_off_timer) {
        esp_timer_stop(s_idle_off_timer);
    }

    if (!wifi_active) {
        xSemaphoreGiveRecursive(s_wifi_mutex);
        return ESP_OK;
    }
    wifi_active = false;
    bool was_ap = ap_active;
    ap_active = false;

    if (esp_sntp_enabled()) {
        esp_sntp_stop();
    }

    if (!was_ap) {
        (void)esp_wifi_scan_stop();
        (void)esp_wifi_disconnect();
    }
    (void)esp_wifi_stop();

    // Allow LwIP & Wi-Fi driver tasks to flush queued frames cleanly before deinit
    vTaskDelay(pdMS_TO_TICKS(60));

    if (instance_any_id) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id);
        instance_any_id = NULL;
    }
    if (instance_got_ip) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip);
        instance_got_ip = NULL;
    }

    // CRITICAL ESP-IDF ORDER: deinit Wi-Fi driver BEFORE destroying default netifs!
    (void)esp_wifi_deinit();

    if (sta_netif != NULL) {
        esp_netif_destroy_default_wifi(sta_netif);
        sta_netif = NULL;
    }
    if (ap_netif != NULL) {
        esp_netif_destroy_default_wifi(ap_netif);
        ap_netif = NULL;
    }

    current_state = WIFI_STATE_OFF;
    ap_count = 0;
    ESP_LOGI(TAG, "WiFi hardware stopped and memory freed cleanly");
    wifi_print_telemetry("STOPPED");
    xSemaphoreGiveRecursive(s_wifi_mutex);
    ble_manager_ensure_advertising();
    return ESP_OK;
}

void wifi_manager_refresh_ps(void)
{
    ensure_wifi_mutex();
    if (xSemaphoreTakeRecursive(s_wifi_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (wifi_active && !ap_active) {
            (void)esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        }
        xSemaphoreGiveRecursive(s_wifi_mutex);
    }
}

void wifi_manager_set_turbo(bool enable)
{
    ensure_wifi_mutex();
    if (xSemaphoreTakeRecursive(s_wifi_mutex, pdMS_TO_TICKS(150)) == pdTRUE) {
        if (wifi_active && !ap_active) {
            // ESP-IDF RF Coexistence rule: WIFI_PS_NONE is forbidden while BLE is active!
            wifi_ps_type_t target = (enable && !ble_manager_is_active()) ? WIFI_PS_NONE : WIFI_PS_MIN_MODEM;
            (void)esp_wifi_set_ps(target);
        }
        xSemaphoreGiveRecursive(s_wifi_mutex);
    }
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
    if (!wifi_active || ap_active) return ESP_ERR_WIFI_NOT_STARTED;

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
    ensure_wifi_mutex();
    xSemaphoreTakeRecursive(s_wifi_mutex, portMAX_DELAY);
    if (wifi_active && !ap_active) {
        wifi_config_t wifi_config = {0};
        strncpy((char *)wifi_config.sta.ssid, a->ssid, sizeof(wifi_config.sta.ssid) - 1);
        if (strlen(a->password) > 0) {
            strncpy((char *)wifi_config.sta.password, a->password, sizeof(wifi_config.sta.password) - 1);
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
    }
    xSemaphoreGiveRecursive(s_wifi_mutex);
    free(a);
    vTaskDelete(NULL);
}

esp_err_t wifi_manager_connect(const char* ssid, const char* password)
{
    if (!wifi_active || ap_active) return ESP_ERR_WIFI_NOT_STARTED;

    current_state = WIFI_STATE_CONNECTING;
    wifi_print_telemetry("START / CONNECTING");

    conn_args_t* args = (conn_args_t*)malloc(sizeof(conn_args_t));
    if (!args) return ESP_ERR_NO_MEM;
    strncpy(args->ssid, ssid, sizeof(args->ssid) - 1);
    args->ssid[sizeof(args->ssid) - 1] = '\0';
    if (password) {
        strncpy(args->password, password, sizeof(args->password) - 1);
        args->password[sizeof(args->password) - 1] = '\0';
    } else {
        args->password[0] = '\0';
    }

    xTaskCreate(wifi_connect_task_func, "wifi_connect", 4096, args, 5, NULL);
    return ESP_OK;
}

bool wifi_manager_is_connected(void)
{
    return (wifi_active && !ap_active && current_state == WIFI_STATE_GOT_IP);
}

void wifi_manager_get_ip(char* buf)
{
    if (buf == NULL) return;
    buf[0] = '\0';
    if (ap_active && ap_netif != NULL) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(ap_netif, &ip_info) == ESP_OK) {
            sprintf(buf, IPSTR, IP2STR(&ip_info.ip));
            return;
        }
    }
    if (wifi_active && current_state == WIFI_STATE_GOT_IP && sta_netif != NULL) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK) {
            sprintf(buf, IPSTR, IP2STR(&ip_info.ip));
        }
    }
}

esp_err_t wifi_manager_connect_default(void)
{
    ensure_wifi_mutex();
    xSemaphoreTakeRecursive(s_wifi_mutex, portMAX_DELAY);

    if (ap_active) {
        wifi_manager_stop();
    }
    if (!wifi_active) {
        esp_err_t ret = wifi_manager_start();
        xSemaphoreGiveRecursive(s_wifi_mutex);
        return ret;
    }
    s_retry_count = 0;
    current_state = WIFI_STATE_CONNECTING;
    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, DEFAULT_WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, DEFAULT_WIFI_PASSWORD, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    ESP_LOGI(TAG, "Connecting to default Wi-Fi: %s...", DEFAULT_WIFI_SSID);
    esp_err_t err = esp_wifi_connect();
    xSemaphoreGiveRecursive(s_wifi_mutex);
    return err;
}

void wifi_manager_get_ssid(char* buf)
{
    if (buf == NULL) return;
    buf[0] = '\0';
    if (!wifi_active) return;
    if (ap_active) {
        strncpy(buf, "UZ_WATCH_AirDrop", 32);
        buf[32] = '\0';
        return;
    }
    wifi_config_t conf = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &conf) == ESP_OK && strlen((char*)conf.sta.ssid) > 0) {
        strncpy(buf, (char*)conf.sta.ssid, 32);
        buf[32] = '\0';
    } else {
        strncpy(buf, DEFAULT_WIFI_SSID, 32);
        buf[32] = '\0';
    }
}

esp_err_t wifi_manager_start_ap(const char* ssid, const char* password)
{
    ensure_wifi_mutex();
    xSemaphoreTakeRecursive(s_wifi_mutex, portMAX_DELAY);

    if (wifi_active && ap_active) {
        ESP_LOGI(TAG, "SoftAP already active");
        xSemaphoreGiveRecursive(s_wifi_mutex);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Starting Wi-Fi Hotspot (SoftAP) mode...");
    if (wifi_active) {
        wifi_manager_stop();
    }
    s_wifi_session_id++;

    if (!nvs_initialized) {
        wifi_manager_pre_init();
    }

    esp_err_t err = ensure_netif_and_loop();
    if (err != ESP_OK) {
        current_state = WIFI_STATE_FAILED;
        xSemaphoreGiveRecursive(s_wifi_mutex);
        return err;
    }

    if (ap_netif == NULL) {
        ap_netif = esp_netif_create_default_wifi_ap();
    }

    wifi_print_telemetry("AP_INIT_BEFORE");

    // Keep STATIC TX buffers (tx_buf_type = 0) — mandatory when Octal PSRAM is active!
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = 0;
    cfg.static_rx_buf_num = 6;
    cfg.dynamic_rx_buf_num = 12;
    cfg.static_tx_buf_num = 8;

    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi SoftAP init failed: %s", esp_err_to_name(err));
        current_state = WIFI_STATE_FAILED;
        xSemaphoreGiveRecursive(s_wifi_mutex);
        return err;
    }

    if (instance_any_id == NULL) {
        err = esp_event_handler_instance_register(WIFI_EVENT,
                                                  ESP_EVENT_ANY_ID,
                                                  &wifi_event_handler,
                                                  NULL,
                                                  &instance_any_id);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "SoftAP event register failed: %s", esp_err_to_name(err));
            esp_wifi_deinit();
            current_state = WIFI_STATE_FAILED;
            xSemaphoreGiveRecursive(s_wifi_mutex);
            return err;
        }
    }

    ap_active = true;
    wifi_active = true;
    current_state = WIFI_STATE_INIT;

    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode(AP) failed: %s", esp_err_to_name(err));
        wifi_manager_stop();
        current_state = WIFI_STATE_FAILED;
        xSemaphoreGiveRecursive(s_wifi_mutex);
        return err;
    }

    wifi_config_t ap_config = {0};
    const char *ap_ssid = (ssid && strlen(ssid) > 0) ? ssid : "UZ_WATCH_AirDrop";
    strncpy((char *)ap_config.ap.ssid, ap_ssid, sizeof(ap_config.ap.ssid) - 1);
    ap_config.ap.ssid_len = strlen((const char *)ap_config.ap.ssid);
    ap_config.ap.channel = 6;
    ap_config.ap.max_connection = 4;
    ap_config.ap.beacon_interval = 100;
    ap_config.ap.pmf_cfg.required = false;

    if (password && strlen(password) >= 8) {
        strncpy((char *)ap_config.ap.password, password, sizeof(ap_config.ap.password) - 1);
        ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    err = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config(AP) failed: %s", esp_err_to_name(err));
        wifi_manager_stop();
        current_state = WIFI_STATE_FAILED;
        xSemaphoreGiveRecursive(s_wifi_mutex);
        return err;
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start(AP) failed: %s", esp_err_to_name(err));
        wifi_manager_stop();
        current_state = WIFI_STATE_FAILED;
        xSemaphoreGiveRecursive(s_wifi_mutex);
        return err;
    }

    // Cap max TX power to 13 dBm for AMOLED & battery protection
    esp_wifi_set_max_tx_power(52);

    current_state = WIFI_STATE_GOT_IP;
    wifi_print_telemetry("AP_STARTED");

    ESP_LOGI(TAG, "SoftAP started! SSID: %s, IP: 192.168.4.1, Auth: %s", ap_ssid,
             (ap_config.ap.authmode == WIFI_AUTH_OPEN) ? "OPEN" : "WPA2");
    xSemaphoreGiveRecursive(s_wifi_mutex);
    return ESP_OK;
}

bool wifi_manager_is_ap_active(void)
{
    return ap_active;
}

static void wifi_async_worker_loop(void* arg) {
    wifi_async_cmd_t cmd;
    while (1) {
        if (xQueueReceive(s_wifi_async_queue, &cmd, portMAX_DELAY) == pdTRUE) {
            if (cmd == WIFI_ASYNC_START) {
                wifi_manager_start();
            } else if (cmd == WIFI_ASYNC_STOP) {
                wifi_manager_stop();
            } else if (cmd == WIFI_ASYNC_CONNECT_DEFAULT) {
                wifi_manager_connect_default();
            }
        }
    }
}

static void dispatch_wifi_async(wifi_async_cmd_t cmd) {
    if (s_wifi_async_queue == NULL) {
        s_wifi_async_queue = xQueueCreate(4, sizeof(wifi_async_cmd_t));
    }
    if (s_wifi_async_task == NULL && s_wifi_async_queue != NULL) {
        xTaskCreatePinnedToCoreWithCaps(
            wifi_async_worker_loop,
            "wifi_async",
            4096,
            NULL,
            4,
            &s_wifi_async_task,
            0,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
        );
    }
    if (s_wifi_async_queue != NULL) {
        xQueueSend(s_wifi_async_queue, &cmd, 0);
    }
}

void wifi_manager_start_async(void) {
    dispatch_wifi_async(WIFI_ASYNC_START);
}

void wifi_manager_stop_async(void) {
    dispatch_wifi_async(WIFI_ASYNC_STOP);
}

void wifi_manager_connect_default_async(void) {
    dispatch_wifi_async(WIFI_ASYNC_CONNECT_DEFAULT);
}
