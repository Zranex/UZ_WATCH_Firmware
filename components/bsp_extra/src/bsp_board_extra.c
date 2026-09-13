#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "driver/i2c.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_event.h"
#include "esp_timer.h"

#include "bsp/esp-bsp.h"
#include "bsp_board_extra.h"
#include "pcf85063a.h"
#include "wifi_manager.h"
#include "qmi8658.h"

#define CONFIG_I2C_MASTER_FREQUENCY 400000
#define I2C_MASTER_TIMEOUT_MS 50

static const char *TAG = "bsp_extra_board";

static i2c_master_bus_handle_t bus_handle;

static i2c_master_dev_handle_t rtc_dev_handle = NULL;

esp_err_t bsp_rtc_init(void)
{
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x51,
        .scl_speed_hz = CONFIG_I2C_MASTER_FREQUENCY,
        .scl_wait_us = 0,
        .flags = {
            .disable_ack_check = 0
        }
    };

    i2c_master_bus_add_device(bus_handle, &dev_config, &rtc_dev_handle);

    return ESP_OK;
}

// read function using new API
int rtc_register_read(uint8_t regAddr, uint8_t *data, uint8_t len) {
    esp_err_t ret = i2c_master_transmit_receive(rtc_dev_handle, &regAddr, 1, data, len, I2C_MASTER_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "RTC READ FAILED!");
        return -1;
    }
    return 0;
}

// write function using new API
int rtc_register_write(uint8_t regAddr, uint8_t *data, uint8_t len) {
    uint8_t *buffer = (uint8_t *)malloc(len + 1);
    if (!buffer) return -1;
    buffer[0] = regAddr;
    memcpy(&buffer[1], data, len);

    esp_err_t ret = i2c_master_transmit(rtc_dev_handle, buffer, len + 1, I2C_MASTER_TIMEOUT_MS);
    free(buffer);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "RTC WRITE FAILED!");
        return -1;
    }
    return 0;
}

// AXP2101 Battery Telemetry
static i2c_master_dev_handle_t axp_dev_handle = NULL;

esp_err_t bsp_battery_init(void) {
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x34, // AXP2101 I2C Address
        .scl_speed_hz = CONFIG_I2C_MASTER_FREQUENCY,
        .scl_wait_us = 0,
        .flags = { .disable_ack_check = 0 }
    };
    esp_err_t ret = i2c_master_bus_add_device(bus_handle, &dev_config, &axp_dev_handle);
    if(ret != ESP_OK) return ret;

    // AXP2101 Power & ADC configuration:
    // 1. Register 0x30 (ADC Channel Control):
    // Enable Batt (bit 5), Sys (bit 4), VBUS (bit 3), Temp (bit 2), Fuel gauge (bit 0), DISABLE TS Pin (bit 1 = 0)
    uint8_t reg_0x30_data[2] = {0x30, 0x3D};
    (void)i2c_master_transmit(axp_dev_handle, reg_0x30_data, 2, I2C_MASTER_TIMEOUT_MS);

    // 2. Register 0x62 (ICC Charge Set):
    // Set 400mA fast charge for 500mAh LiPo (0x08 = 400mA)
    uint8_t reg_0x62_data[2] = {0x62, 0x08};
    (void)i2c_master_transmit(axp_dev_handle, reg_0x62_data, 2, I2C_MASTER_TIMEOUT_MS);

    ESP_LOGI(TAG, "AXP2101 PMIC configured (ADC active, TS disabled, 400mA charging)");
    return ESP_OK;
}

static volatile int s_cached_battery_percent = -1;
static volatile bool s_cached_battery_charging = false;
static int64_t s_last_battery_read_us = 0;
static int64_t s_last_charging_read_us = 0;

int bsp_battery_get_percent(void) {
    if (!axp_dev_handle) return 100;
    
    int64_t now = esp_timer_get_time();
    // Cache for 3 seconds to avoid blocking the LVGL UI thread with frequent I2C traffic
    if (s_cached_battery_percent >= 0 && (now - s_last_battery_read_us < 3000000)) {
        return s_cached_battery_percent;
    }

    uint8_t reg = 0xA4; // Battery percentage register
    uint8_t percent = 0;
    esp_err_t ret = i2c_master_transmit_receive(axp_dev_handle, &reg, 1, &percent, 1, 20);
    if (ret == ESP_OK) {
        s_last_battery_read_us = now;
        // On AXP2101, reg 0xA4 provides percentage directly (0-100), or masked with 0x7F
        int val = (percent <= 100) ? (int)percent : (int)(percent & 0x7F);
        if (val >= 0 && val <= 100) {
            s_cached_battery_percent = val;
            return val;
        }
    }
    return (s_cached_battery_percent >= 0) ? s_cached_battery_percent : 100;
}

bool bsp_battery_is_charging(void) {
    if (!axp_dev_handle) return false;
    
    int64_t now = esp_timer_get_time();
    if (now - s_last_charging_read_us < 3000000) {
        return s_cached_battery_charging;
    }

    uint8_t reg = 0x01; // Power status 2 (AXP2101)
    uint8_t status2 = 0;
    esp_err_t ret = i2c_master_transmit_receive(axp_dev_handle, &reg, 1, &status2, 1, 20);
    if (ret == ESP_OK) {
        s_last_charging_read_us = now;
        // AXP2101 Reg 0x01 Bits [6:5]: 01 = Charging
        s_cached_battery_charging = (((status2 >> 5) & 0x03) == 0x01);
        return s_cached_battery_charging;
    }
    return s_cached_battery_charging;
}

esp_err_t bsp_extra_init(void)
{
    esp_err_t ret;

    // Ensure default event loop exists for cross-component events
    //(void)esp_event_loop_create_default();

    bus_handle = bsp_i2c_get_handle();
    
    ret = bsp_rtc_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "RTC init failed");
        return ret;
    }

    ret = pcf85063a_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PCF85063A init failed");
        return ret;
    }   

    ret = qmi8658_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "QMI8658 init failed");
        // Not returning error to let it boot even if IMU fails
    }

    ret = wifi_manager_pre_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "wifi_manager_pre_init failed");
        // Don't return error here, to let the system boot even if NVS fails
    }

    ret = bsp_battery_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Battery AXP2101 init failed");
    }

    return ESP_OK;
}
