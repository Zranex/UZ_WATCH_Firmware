#include "ble_manager.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "host/ble_hs.h"
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
// Custom Characteristic UUID: 0xFF01
static const ble_uuid16_t time_chr_uuid = BLE_UUID16_INIT(0xFF01);

static int ble_time_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt,
                               void *arg);

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
                0, // No more characteristics
            }
        },
    },
    {
        0, // No more services
    },
};

static int ble_time_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt,
                               void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        // We expect a string like "1723824000" (Unix timestamp) or raw 32-bit int
        // Let's assume it's a string timestamp for ease of use with standard apps
        char buf[32];
        int len = OS_MBUF_PKTLEN(ctxt->om);
        if (len > 0 && len < sizeof(buf)) {
            os_mbuf_copydata(ctxt->om, 0, len, buf);
            buf[len] = '\0';
            
            long int timestamp = strtol(buf, NULL, 10);
            if (timestamp > 0) {
                ESP_LOGI(TAG, "Received Time Sync: %ld", timestamp);
                
                // Update RTC
                struct tm timeinfo;
                time_t t = (time_t)timestamp;
                // Add timezone offset here if needed (e.g., UTC+3 for Turkey)
                // We'll just convert local string to local time directly.
                // Assuming the app sends the correct local unix timestamp.
                localtime_r(&t, &timeinfo);
                rtc_set_time(&timeinfo);
                
                ESP_LOGI(TAG, "Time updated to: %02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
            } else {
                ESP_LOGW(TAG, "Invalid time format received: %s", buf);
            }
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
        if (event->connect.status != 0) {
            // Connection failed, resume advertising
            ble_hs_id_infer_auto(0, NULL); // We don't care about addr type here directly
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE Disconnected. Resuming advertising.");
        // Resume advertising
        struct ble_gap_adv_params adv_params;
        memset(&adv_params, 0, sizeof(adv_params));
        adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
        adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
        uint8_t own_addr_type;
        ble_hs_id_infer_auto(0, &own_addr_type);
        ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, NULL, NULL);
        break;
    }
    return 0;
}

static void ble_app_on_sync(void)
{
    esp_err_t rc;

    const char *dev_name = "UZ WATCH";
    rc = ble_svc_gap_device_name_set(dev_name);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to set device name, rc=%d", rc);
        return;
    }

    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof fields);
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.name = (uint8_t *)dev_name;
    fields.name_len = strlen(dev_name);
    fields.name_is_complete = 1;
    
    // Also include custom service UUID in advertising data
    fields.uuids16 = (ble_uuid16_t[]){ time_service_uuid };
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "Error setting advertisement data; rc=%d", rc);
        return;
    }

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    uint8_t own_addr_type;
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "Error determining address type, rc=%d", rc);
        return;
    }

    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, ble_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Error starting advertising, rc=%d", rc);
        return;
    }

    ESP_LOGI(TAG, "Advertising started as UZ WATCH");
}

static void ble_host_task(void *param)
{
    ESP_LOGI(TAG, "BLE Host Task Started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_manager_init(void)
{
    ESP_LOGI(TAG, "Initializing BLE Manager (NimBLE)");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    nimble_port_init();

    // Initialize GATT server
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

    ble_hs_cfg.sync_cb = ble_app_on_sync;
    
    // Set MTU for large strings if needed
    // ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO; // No auth
    
    nimble_port_freertos_init(ble_host_task);

    return ESP_OK;
}

