#include "ble_manager.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "host/ble_hs.h"
#include "host/ble_att.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "rtc_lib.h"
#include <string.h>
#include <stdlib.h>
#include "os/os_mbuf.h"

static const char *TAG = "ble_manager";

// Custom Service UUID: 0x00FF
static const ble_uuid16_t time_service_uuid = BLE_UUID16_INIT(0x00FF);
// Custom Characteristic UUID: 0xFF01 (Time)
static const ble_uuid16_t time_chr_uuid = BLE_UUID16_INIT(0xFF01);
// Custom Characteristic UUID: 0xFF02 (Media Info RX)
static const ble_uuid16_t media_chr_uuid = BLE_UUID16_INIT(0xFF02);
// Custom Characteristic UUID: 0xFF03 (Media Command TX - Notify)
static const ble_uuid16_t media_cmd_chr_uuid = BLE_UUID16_INIT(0xFF03);
// Custom Characteristic UUID: 0xFF04 (Notification Info RX)
static const ble_uuid16_t notif_chr_uuid = BLE_UUID16_INIT(0xFF04);

static uint16_t media_cmd_handle;
static volatile uint16_t ble_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_ble_active = false;
static volatile bool s_ble_softap_paused = false;
static bool s_nimble_started = false;

static void ble_app_advertise(void);
static void start_nimble_stack(void);

// External app callbacks (ALL executed safely on ble_evt_wrk, NEVER on nimble_host!)
extern void app_media_player_update_from_ble(const char* payload);
extern void app_notifications_show_from_ble(const char* payload);
extern void app_call_manager_show_incoming_call(const char* caller_name);
extern void app_call_manager_hide_incoming_call(void);
extern void app_weather_update_from_ble(const char* city, const char* temp, const char* condition);
extern void app_weather_update_advanced_from_ble(const char* city, const char* temp, const char* condition,
                                                 const char* d0, const char* d1, const char* d2);
extern void app_agenda_update_from_ble(const char* payload);
extern void app_transit_update_from_ble(const char* payload);
extern void app_notifications_update_quick_replies(const char* payload);
extern void app_notifications_update_nav_from_ble(const char* payload);
extern void app_notifications_hide_nav_from_ble(void);
extern void bsp_vibrate_pulse(uint32_t duration_ms);
extern void app_notifications_show_system_alert(const char* msg);
extern void app_lockscreen_reset_to_default(void);
extern void app_local_music_play_from_ble(const char* song_name);
extern void app_find_watch_start_alarm(void);
extern void app_find_watch_stop_alarm(void);
extern void app_tars_set_api_key_from_ble(const char* key);
extern void app_tars_handle_reply_from_ble(const char* payload);

// Asynchronous BLE Event Worker (protects nimble_host from stack overflow, LVGL/display deadlocks, SD/NVS stalls)
typedef enum {
    BLE_WORK_TIME = 1,
    BLE_WORK_MEDIA = 2,
    BLE_WORK_NOTIF = 3,
    BLE_WORK_DISCONNECT_ALERT = 4,
    BLE_WORK_HIDE_NAV_SILENT = 5,
    BLE_WORK_SET_ACTIVE = 6,
    BLE_WORK_SEND_NOTIFY = 7,
} ble_work_type_t;

typedef struct {
    uint8_t type;
    char payload[256];
} ble_work_item_t;

static QueueHandle_t s_ble_work_queue = NULL;
static TaskHandle_t s_ble_work_task = NULL;
static SemaphoreHandle_t s_ble_api_mutex = NULL;

static void handle_notif_payload(char *buf)
{
    if (strcmp(buf, "FIND_PHONE_STOP_ACK") == 0 || strncmp(buf, "FIND_PHONE_STOP_ACK|", 20) == 0) {
        app_find_phone_handle_stop_ack();
    } else if (strcmp(buf, "FIND_WATCH") == 0 || strncmp(buf, "FIND_WATCH|", 11) == 0) {
        app_find_watch_start_alarm();
    } else if (strcmp(buf, "FIND_WATCH_STOP") == 0 || strncmp(buf, "FIND_WATCH_STOP|", 16) == 0) {
        app_find_watch_stop_alarm();
    } else if (strncmp(buf, "TARS_KEY|", 9) == 0) {
        app_tars_set_api_key_from_ble(buf + 9);
    } else if (strncmp(buf, "TARS_REPLY|", 11) == 0) {
        app_tars_handle_reply_from_ble(buf + 11);
    } else if (strncmp(buf, "QREPLIES|", 9) == 0) {
        app_notifications_update_quick_replies(buf + 9);
    } else if (strncmp(buf, "NAV|", 4) == 0) {
        app_notifications_update_nav_from_ble(buf + 4);
    } else if (strcmp(buf, "NAV_END") == 0 || strncmp(buf, "NAV_END|", 8) == 0) {
        app_notifications_hide_nav_from_ble();
    } else if (strcmp(buf, "RESET_WALLPAPER") == 0 || strncmp(buf, "RESET_WALLPAPER|", 16) == 0) {
        app_lockscreen_reset_to_default();
        bsp_vibrate_pulse(80);
        app_notifications_show_system_alert("Varsayilan ekrana donuldu");
    } else if (strncmp(buf, "CALL|", 5) == 0) {
        app_call_manager_show_incoming_call(buf + 5);
    } else if (strcmp(buf, "CALL_END") == 0) {
        app_call_manager_hide_incoming_call();
    } else if (strncmp(buf, "AGENDA|", 7) == 0) {
        app_agenda_update_from_ble(buf + 7);
    } else if (strncmp(buf, "TRANSIT_RESP|", 13) == 0) {
        app_transit_update_from_ble(buf + 13);
    } else if (strncmp(buf, "WEATHER2|", 9) == 0) {
        char *payload = buf + 9;
        char *saveptr = NULL;
        char *city = strtok_r(payload, "|", &saveptr);
        char *temp = strtok_r(NULL, "|", &saveptr);
        char *condition = strtok_r(NULL, "|", &saveptr);
        char *d0 = strtok_r(NULL, "|", &saveptr);
        char *d1 = strtok_r(NULL, "|", &saveptr);
        char *d2 = strtok_r(NULL, "|", &saveptr);
        if (city && temp && condition && d0 && d1 && d2) {
            app_weather_update_advanced_from_ble(city, temp, condition, d0, d1, d2);
        } else if (city && temp && condition) {
            app_weather_update_from_ble(city, temp, condition);
        }
    } else if (strncmp(buf, "WEATHER|", 8) == 0) {
        char *payload = buf + 8;
        char *saveptr = NULL;
        char *city = strtok_r(payload, "|", &saveptr);
        char *temp = strtok_r(NULL, "|", &saveptr);
        char *condition = strtok_r(NULL, "|", &saveptr);
        if (city && temp && condition) {
            app_weather_update_from_ble(city, temp, condition);
        }
    } else {
        app_notifications_show_from_ble(buf);
    }
}

static void ble_worker_task(void *arg)
{
    ESP_LOGI(TAG, "BLE Event Worker Task started (6KB internal stack on Core 1)");
    ble_work_item_t *item = NULL;
    while (1) {
        if (xQueueReceive(s_ble_work_queue, &item, pdMS_TO_TICKS(15000)) == pdTRUE && item != NULL) {
            switch (item->type) {
            case BLE_WORK_TIME: {
                long int timestamp = strtol(item->payload, NULL, 10);
                if (timestamp > 0) {
                    ESP_LOGI(TAG, "Received Time Sync: %ld", timestamp);
                    struct tm timeinfo;
                    time_t t = (time_t)timestamp;
                    localtime_r(&t, &timeinfo);
                    rtc_set_time(&timeinfo);
                    ESP_LOGI(TAG, "Time updated to: %02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
                } else {
                    ESP_LOGW(TAG, "Invalid time format received: %s", item->payload);
                }
                break;
            }
            case BLE_WORK_MEDIA:
                if (strncmp(item->payload, "WATCH_PLAY|", 11) == 0) {
                    ESP_LOGI(TAG, "Received Local Play Command: %s", item->payload + 11);
                    app_local_music_play_from_ble(item->payload + 11);
                } else {
                    ESP_LOGI(TAG, "Received Media Sync: %s", item->payload);
                    app_media_player_update_from_ble(item->payload);
                }
                break;
            case BLE_WORK_NOTIF:
                ESP_LOGI(TAG, "Processing Notification/Command: %s", item->payload);
                handle_notif_payload(item->payload);
                break;
            case BLE_WORK_DISCONNECT_ALERT:
                app_notifications_hide_nav_from_ble();
                if (s_ble_active && !s_ble_softap_paused) {
                    app_notifications_show_system_alert("Telefon Baglantisi Koptu!");
                }
                break;
            case BLE_WORK_HIDE_NAV_SILENT:
                app_notifications_hide_nav_from_ble();
                break;
            case BLE_WORK_SET_ACTIVE: {
                bool enable = (item->payload[0] == '1');
                nvs_handle_t nvs_h;
                if (nvs_open("settings", NVS_READWRITE, &nvs_h) == ESP_OK) {
                    nvs_set_u8(nvs_h, "ble_en", enable ? 1 : 0);
                    nvs_commit(nvs_h);
                    nvs_close(nvs_h);
                    ESP_LOGI(TAG, "Saved BLE state to NVS: %d", enable ? 1 : 0);
                }
                if (!enable) {
                    ESP_LOGI(TAG, "Disabling BLE (Radio sleep/power save)...");
                    uint16_t h = ble_conn_handle;
                    if (h != BLE_HS_CONN_HANDLE_NONE) {
                        ble_gap_terminate(h, BLE_ERR_REM_USER_CONN_TERM);
                        ble_conn_handle = BLE_HS_CONN_HANDLE_NONE;
                    }
                    ble_gap_adv_stop();
                    ESP_LOGI(TAG, "BLE advertising stopped.");
                } else {
                    ESP_LOGI(TAG, "Enabling BLE...");
                    if (!s_nimble_started) {
                        start_nimble_stack();
                    } else if (!s_ble_softap_paused) {
                        ble_app_advertise();
                    }
                }
                break;
            }
            case BLE_WORK_SEND_NOTIFY: {
                uint16_t h = ble_conn_handle;
                if (s_ble_active && !s_ble_softap_paused && h != BLE_HS_CONN_HANDLE_NONE && item->payload[0] != '\0') {
                    size_t len = strlen(item->payload);
                    uint16_t mtu = ble_att_mtu(h);
                    size_t max_payload = (mtu > 3) ? (size_t)(mtu - 3) : 20;
                    if (len > max_payload) {
                        len = max_payload;
                        // Avoid splitting a multi-byte UTF-8 character at the boundary
                        while (len > 0 && ((unsigned char)item->payload[len] & 0xC0) == 0x80) {
                            len--;
                        }
                    }
                    if (len > 0) {
                        struct os_mbuf *om = ble_hs_mbuf_from_flat(item->payload, (uint16_t)len);
                        if (om) {
                            ESP_LOGI(TAG, "Sending BLE Notify (%u/%u B): %.*s",
                                     (unsigned)len, (unsigned)max_payload, (int)len, item->payload);
                            int rc = ble_gatts_notify_custom(h, media_cmd_handle, om);
                            if (rc != 0) {
                                ESP_LOGW(TAG, "ble_gatts_notify_custom rc=%d", rc);
                            }
                        }
                    }
                }
                break;
            }
            default:
                break;
            }
            free(item);
            item = NULL;
        } else {
            // Periodic 15s self-healing watchdog: ensure BLE stays discoverable if idle
            ble_manager_ensure_advertising();
        }
    }
}

static void ensure_ble_worker(void)
{
    if (s_ble_api_mutex == NULL) {
        s_ble_api_mutex = xSemaphoreCreateRecursiveMutex();
    }
    if (s_ble_work_queue == NULL) {
        s_ble_work_queue = xQueueCreate(20, sizeof(ble_work_item_t *));
    }
    if (s_ble_work_task == NULL && s_ble_work_queue != NULL) {
        xTaskCreatePinnedToCoreWithCaps(
            ble_worker_task,
            "ble_evt_wrk",
            6144,
            NULL,
            4,
            &s_ble_work_task,
            1,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
        );
    }
}

static void ble_enqueue_work(ble_work_type_t type, const char *payload)
{
    ensure_ble_worker();
    if (!s_ble_work_queue) return;

    ble_work_item_t *item = (ble_work_item_t *)heap_caps_malloc(sizeof(ble_work_item_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!item) {
        item = (ble_work_item_t *)malloc(sizeof(ble_work_item_t));
    }
    if (!item) return;

    item->type = (uint8_t)type;
    if (payload) {
        strncpy(item->payload, payload, sizeof(item->payload) - 1);
        item->payload[sizeof(item->payload) - 1] = '\0';
    } else {
        item->payload[0] = '\0';
    }

    if (xQueueSend(s_ble_work_queue, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "BLE work queue full, dropping event type %d", (int)type);
        free(item);
    }
}

static int ble_time_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg);
static int ble_media_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg);
static int ble_notif_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg);

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &time_service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &time_chr_uuid.u,
                .access_cb = ble_time_chr_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &media_chr_uuid.u,
                .access_cb = ble_media_chr_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &media_cmd_chr_uuid.u,
                .access_cb = ble_media_chr_access,
                .flags = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
                .val_handle = &media_cmd_handle,
            },
            {
                .uuid = &notif_chr_uuid.u,
                .access_cb = ble_notif_chr_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                0,
            }
        },
    },
    {
        0,
    },
};

static int ble_time_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        char buf[32];
        int len = OS_MBUF_PKTLEN(ctxt->om);
        if (len > 0 && len < (int)sizeof(buf)) {
            os_mbuf_copydata(ctxt->om, 0, len, buf);
            buf[len] = '\0';
            ble_enqueue_work(BLE_WORK_TIME, buf);
        }
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static int ble_media_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        char buf[128];
        int len = OS_MBUF_PKTLEN(ctxt->om);
        if (len > 0 && len < (int)sizeof(buf)) {
            os_mbuf_copydata(ctxt->om, 0, len, buf);
            buf[len] = '\0';
            ble_enqueue_work(BLE_WORK_MEDIA, buf);
        }
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static int ble_notif_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        char buf[256];
        int len = OS_MBUF_PKTLEN(ctxt->om);
        if (len > 0 && len < (int)sizeof(buf)) {
            os_mbuf_copydata(ctxt->om, 0, len, buf);
            buf[len] = '\0';
            ble_enqueue_work(BLE_WORK_NOTIF, buf);
        }
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static int ble_gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "BLE Connected! Status: %d", event->connect.status);
        if (event->connect.status == 0) {
            ble_conn_handle = event->connect.conn_handle;

            // Coexistence-hardened BLE connection parameters (6s supervision timeout prevents Wi-Fi STA drops)
            struct ble_gap_upd_params conn_params = {
                .itvl_min = 40,   // 50ms
                .itvl_max = 80,   // 100ms
                .latency = 3,     // Optimal battery saving + fast responsiveness
                .supervision_timeout = 600, // 6000ms (6s) for rock-solid Wi-Fi+BLE coex
                .min_ce_len = 0,
                .max_ce_len = 0,
            };
            int rc = ble_gap_update_params(event->connect.conn_handle, &conn_params);
            ESP_LOGI(TAG, "Requested coex-hardened BLE conn params: rc=%d", rc);
        } else {
            ble_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            if (s_ble_active && !s_ble_softap_paused) {
                ble_app_advertise();
            }
        }
        break;
    case BLE_GAP_EVENT_CONN_UPDATE:
        ESP_LOGI(TAG, "BLE connection updated: status=%d", event->conn_update.status);
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE Disconnected (reason=%d, softap_paused=%d).",
                 event->disconnect.reason, (int)s_ble_softap_paused);
        ble_conn_handle = BLE_HS_CONN_HANDLE_NONE;

        if (s_ble_active && !s_ble_softap_paused) {
            ble_app_advertise();
            ble_enqueue_work(BLE_WORK_DISCONNECT_ALERT, NULL);
        } else {
            ble_enqueue_work(BLE_WORK_HIDE_NAV_SILENT, NULL);
        }
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGW(TAG, "BLE Advertising completed unexpectedly (reason=%d), restarting...",
                 event->adv_complete.reason);
        if (s_ble_active && !s_ble_softap_paused && ble_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
            ble_app_advertise();
        }
        break;
    }
    return 0;
}

static void ble_app_advertise(void)
{
    if (!s_ble_active || s_ble_softap_paused || !s_nimble_started) return;
    bool locked = (s_ble_api_mutex && xSemaphoreTakeRecursive(s_ble_api_mutex, pdMS_TO_TICKS(200)) == pdTRUE);
    if (s_ble_api_mutex && !locked) return;

    if (!s_ble_active || s_ble_softap_paused || !s_nimble_started ||
        ble_conn_handle != BLE_HS_CONN_HANDLE_NONE || ble_gap_adv_active()) {
        if (locked) xSemaphoreGiveRecursive(s_ble_api_mutex);
        return;
    }

    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof fields);
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    const char *dev_name = "UZ WATCH";
    fields.name = (uint8_t *)dev_name;
    fields.name_len = strlen(dev_name);
    fields.name_is_complete = 1;

    fields.uuids16 = (ble_uuid16_t[]){ time_service_uuid };
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "Error setting advertisement data; rc=%d", rc);
        if (locked) xSemaphoreGiveRecursive(s_ble_api_mutex);
        return;
    }

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    adv_params.itvl_min = 160; // 100ms (instant Android discovery, ~0.7mA avg)
    adv_params.itvl_max = 240; // 150ms

    uint8_t own_addr_type;
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "Error determining address type, rc=%d", rc);
        if (locked) xSemaphoreGiveRecursive(s_ble_api_mutex);
        return;
    }

    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, ble_gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "Error starting advertising, rc=%d", rc);
        if (locked) xSemaphoreGiveRecursive(s_ble_api_mutex);
        return;
    }

    ESP_LOGI(TAG, "Advertising started as UZ WATCH");
    if (locked) xSemaphoreGiveRecursive(s_ble_api_mutex);
}

void ble_manager_ensure_advertising(void)
{
    if (!s_ble_active || s_ble_softap_paused || !s_nimble_started) return;
    if (ble_conn_handle != BLE_HS_CONN_HANDLE_NONE) return;
    if (!ble_gap_adv_active()) {
        ESP_LOGW(TAG, "BLE self-heal: advertising was inactive, restarting UZ WATCH...");
        ble_app_advertise();
    }
}

bool ble_manager_is_active(void)
{
    return s_ble_active;
}

bool ble_manager_is_connected(void)
{
    return (s_ble_active && !s_ble_softap_paused && ble_conn_handle != BLE_HS_CONN_HANDLE_NONE);
}

static void ble_app_on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE Host reset; reason=%d (waiting for re-sync)", reason);
    ble_conn_handle = BLE_HS_CONN_HANDLE_NONE;
}

static void ble_app_on_sync(void);
static void ble_host_task(void *param);

static void start_nimble_stack(void)
{
    if (s_nimble_started) return;
    ESP_LOGI(TAG, "Starting NimBLE Controller and Host stack...");
    nimble_port_init();

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to configure GATT services, rc=%d", rc);
    }

    rc = ble_gatts_add_svcs(gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to add GATT services, rc=%d", rc);
    }

    ble_hs_cfg.reset_cb = ble_app_on_reset;
    ble_hs_cfg.sync_cb = ble_app_on_sync;

    nimble_port_freertos_init(ble_host_task);
    s_nimble_started = true;
}

void ble_manager_set_active(bool enable)
{
    if (s_ble_active == enable) return;
    s_ble_active = enable;
    ble_enqueue_work(BLE_WORK_SET_ACTIVE, enable ? "1" : "0");
}

void ble_manager_set_softap_pause(bool pause)
{
    if (s_ble_softap_paused == pause) return;
    s_ble_softap_paused = pause;

    if (!s_nimble_started) return;

    if (pause) {
        ESP_LOGI(TAG, "Pausing BLE radio cleanly for AirDrop SoftAP coexistence...");
        ble_gap_adv_stop();
        uint16_t h = ble_conn_handle;
        if (h != BLE_HS_CONN_HANDLE_NONE) {
            ble_gap_terminate(h, BLE_ERR_REM_USER_CONN_TERM);
            ble_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        }
    } else {
        ESP_LOGI(TAG, "Resuming BLE radio after AirDrop SoftAP...");
        if (s_ble_active && ble_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
            ble_app_advertise();
        }
    }
}

static void ble_app_on_sync(void)
{
    const char *dev_name = "UZ WATCH";
    int rc = ble_svc_gap_device_name_set(dev_name);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to set device name, rc=%d", rc);
        return;
    }

    if (s_ble_active && !s_ble_softap_paused) {
        ble_app_advertise();
    } else {
        ESP_LOGI(TAG, "BLE radio staying idle (active=%d, softap_paused=%d).",
                 (int)s_ble_active, (int)s_ble_softap_paused);
    }
}

static void ble_host_task(void *param)
{
    ESP_LOGI(TAG, "BLE Host Task Started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_manager_init(void)
{
    ESP_LOGI(TAG, "Initializing BLE Manager (NimBLE + Async Worker)");
    ensure_ble_worker();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Always boot with BLE ENABLED so a forgotten power-save toggle never silently hides the watch!
    // (Skip unnecessary NVS write/commit if ble_en is already 1 to avoid boot flash stalls.)
    s_ble_active = true;
    nvs_handle_t nvs_h;
    if (nvs_open("settings", NVS_READWRITE, &nvs_h) == ESP_OK) {
        uint8_t cur_ble = 0;
        if (nvs_get_u8(nvs_h, "ble_en", &cur_ble) != ESP_OK || cur_ble != 1) {
            nvs_set_u8(nvs_h, "ble_en", 1);
            nvs_commit(nvs_h);
        }
        nvs_close(nvs_h);
    }
    ESP_LOGI(TAG, "BLE power state: ENABLED (Auto-Start on Boot)");
    start_nimble_stack();
    return ESP_OK;
}

void ble_manager_send_media_command(const char* command)
{
    if (!command || command[0] == '\0') return;
    if (s_ble_active && !s_ble_softap_paused && ble_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        // Serialize all GATT notifications through ble_evt_wrk with MTU clamp (zero cross-core races!)
        ble_enqueue_work(BLE_WORK_SEND_NOTIFY, command);
    } else {
        ESP_LOGW(TAG, "Cannot send BLE command (not connected or SoftAP active).");
    }
}
