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
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

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

int rtc_register_read(uint8_t regAddr, uint8_t *data, uint8_t len) {
    esp_err_t ret = i2c_master_transmit_receive(rtc_dev_handle, &regAddr, 1, data, len, I2C_MASTER_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "RTC READ FAILED!");
        return -1;
    }
    return 0;
}

int rtc_register_write(uint8_t regAddr, uint8_t *data, uint8_t len) {
    uint8_t buffer[32];
    if (!data || (size_t)len + 1 > sizeof(buffer)) return -1;
    buffer[0] = regAddr;
    memcpy(&buffer[1], data, len);

    esp_err_t ret = i2c_master_transmit(rtc_dev_handle, buffer, len + 1, I2C_MASTER_TIMEOUT_MS);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "RTC WRITE FAILED!");
        return -1;
    }
    return 0;
}

// AXP2101 Battery Telemetry & Haptic Motor (ALDO3 + GPIO18)
static i2c_master_dev_handle_t axp_dev_handle = NULL;
static esp_timer_handle_t s_vib_off_timer = NULL;
static SemaphoreHandle_t s_audio_mutex = NULL;
#define BSP_VIB_MOTOR_GPIO GPIO_NUM_18

bool bsp_audio_lock(uint32_t timeout_ms) {
    if (s_audio_mutex == NULL) {
        s_audio_mutex = xSemaphoreCreateRecursiveMutex();
    }
    if (!s_audio_mutex) return true;
    TickType_t ticks = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return (xSemaphoreTakeRecursive(s_audio_mutex, ticks) == pdTRUE);
}

void bsp_audio_unlock(void) {
    if (s_audio_mutex) {
        xSemaphoreGiveRecursive(s_audio_mutex);
    }
}

static void vib_off_timer_cb(void* arg) {
    gpio_set_level(BSP_VIB_MOTOR_GPIO, 0);
}

void bsp_vibrate_pulse(uint32_t duration_ms) {
    if (!s_vib_off_timer) return;
    if (duration_ms < 20) duration_ms = 20;
    if (duration_ms > 500) duration_ms = 500;
    gpio_set_level(BSP_VIB_MOTOR_GPIO, 1);
    esp_timer_stop(s_vib_off_timer);
    (void)esp_timer_start_once(s_vib_off_timer, (uint64_t)duration_ms * 1000ULL);
}

static bool axp_read_reg(uint8_t reg, uint8_t *out, size_t len) {
    if (!axp_dev_handle) return false;
    return (i2c_master_transmit_receive(axp_dev_handle, &reg, 1, out, len, I2C_MASTER_TIMEOUT_MS) == ESP_OK);
}

void bsp_pmu_log_diagnostics(void) {
    if (!axp_dev_handle) return;
    uint8_t st0 = 0, st1 = 0, soc = 0, dcdc = 0, ldo0 = 0, ldo1 = 0;
    uint8_t vbat_b[2] = {0}, vbus_b[2] = {0}, vsys_b[2] = {0};

    axp_read_reg(0x00, &st0, 1);
    axp_read_reg(0x01, &st1, 1);
    axp_read_reg(0xA4, &soc, 1);
    axp_read_reg(0x80, &dcdc, 1);
    axp_read_reg(0x90, &ldo0, 1);
    axp_read_reg(0x91, &ldo1, 1);
    axp_read_reg(0x34, vbat_b, 2);
    axp_read_reg(0x38, vbus_b, 2);
    axp_read_reg(0x3A, vsys_b, 2);

    int vbat_mv = ((vbat_b[0] & 0x3F) << 8) | vbat_b[1];
    int vbus_mv = ((vbus_b[0] & 0x3F) << 8) | vbus_b[1];
    int vsys_mv = ((vsys_b[0] & 0x3F) << 8) | vsys_b[1];
    bool vbus_good = (st0 & (1 << 5)) != 0;
    bool bat_present = (st0 & (1 << 3)) != 0;
    uint8_t chg_dir = (st1 >> 5) & 0x03; // 00=standby, 01=charging, 10=discharging
    uint8_t chg_st = st1 & 0x07;

    ESP_LOGW(TAG, "=== AXP2101 LIVE TELEMETRY ===");
    ESP_LOGW(TAG, "SOC=%u%% | VBAT=%d mV | VSYS=%d mV | VBUS=%d mV (USB=%s, BAT=%s)",
             soc, vbat_mv, vsys_mv, vbus_mv, vbus_good ? "YES" : "NO", bat_present ? "YES" : "NO");
    ESP_LOGW(TAG, "STAT0=0x%02X STAT1=0x%02X (dir=%u chg_state=%u) | DCDC=0x%02X LDO0=0x%02X LDO1=0x%02X",
             st0, st1, chg_dir, chg_st, dcdc, ldo0, ldo1);
    ESP_LOGW(TAG, "==============================");
}

esp_err_t bsp_battery_init(void) {
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x34, // AXP2101 I2C Address
        .scl_speed_hz = CONFIG_I2C_MASTER_FREQUENCY,
        .scl_wait_us = 0,
        .flags = { .disable_ack_check = 0 }
    };
    esp_err_t ret = i2c_master_bus_add_device(bus_handle, &dev_config, &axp_dev_handle);
    if (ret != ESP_OK) return ret;

    // 1. Register 0x30 (ADC Channel Control):
    // Enable Batt (bit 5), Sys (bit 4), VBUS (bit 3), Temp (bit 2), Fuel gauge (bit 0), DISABLE TS Pin (bit 1 = 0)
    uint8_t reg_0x30_data[2] = {0x30, 0x3D};
    (void)i2c_master_transmit(axp_dev_handle, reg_0x30_data, 2, I2C_MASTER_TIMEOUT_MS);

    // 2. Register 0x62 (ICC Charge Set):
    // Set 300mA safe fast charge for watch LiPo (0x09 = 300mA, gentle on battery health & thermals)
    uint8_t reg_0x62_data[2] = {0x62, 0x09};
    (void)i2c_master_transmit(axp_dev_handle, reg_0x62_data, 2, I2C_MASTER_TIMEOUT_MS);

    // 3. Register 0x64 (CV Charge Voltage): 4.20V (0x03 = 4.2V)
    uint8_t reg_0x64_data[2] = {0x64, 0x03};
    (void)i2c_master_transmit(axp_dev_handle, reg_0x64_data, 2, I2C_MASTER_TIMEOUT_MS);

    // 4. Register 0x24: Set V_OFF cut-off to 3.3V (0x03) / 3.4V (0x04) to prevent deep over-discharge
    uint8_t reg_0x24_data[2] = {0x24, 0x04};
    (void)i2c_master_transmit(axp_dev_handle, reg_0x24_data, 2, I2C_MASTER_TIMEOUT_MS);

    // 5. Register 0x18: Ensure Charger + Fuel Gauge + Cell Battery Charge Control enabled (bit 1 = Charger EN, bit 3 = Gauge EN)
    uint8_t reg18 = 0;
    if (axp_read_reg(0x18, &reg18, 1)) {
        reg18 |= (1 << 1) | (1 << 3);
        uint8_t reg_0x18_data[2] = {0x18, reg18};
        (void)i2c_master_transmit(axp_dev_handle, reg_0x18_data, 2, I2C_MASTER_TIMEOUT_MS);
    }

    // 6. Shut down ALL unconnected AXP2101 rails per PCB schematic:
    //    - DCDC2, DCDC3, DCDC4, DCDC5 are NC -> keep ONLY DCDC1 (VCC3V3) enabled (0x80 = 0x01)
    //    - ALDO4, BLDO1, BLDO2, CPUSLDO, DLDO1, DLDO2 are NC, ALDO3 (Motor) idle OFF -> keep ONLY ALDO1 (A3V3) + ALDO2 (DSI_PWR_EN) ON (0x90 = 0x03, 0x91 = 0x00)
    uint8_t dcdc_cfg = 0;
    if (axp_read_reg(0x80, &dcdc_cfg, 1)) {
        dcdc_cfg = (dcdc_cfg & 0xE0) | 0x01; // Only DCDC1 ON
        uint8_t wr_dcdc[2] = {0x80, dcdc_cfg};
        (void)i2c_master_transmit(axp_dev_handle, wr_dcdc, 2, I2C_MASTER_TIMEOUT_MS);
    }
    uint8_t wr_aldo3_v[2] = {0x94, 0x1C}; // ALDO3 = 3.3V (when motor pulsed)
    (void)i2c_master_transmit(axp_dev_handle, wr_aldo3_v, 2, I2C_MASTER_TIMEOUT_MS);
    uint8_t wr_ldo0[2] = {0x90, 0x07}; // ALDO1 + ALDO2 + ALDO3 ON (GPIO18 NPN switch gates motor current)
    (void)i2c_master_transmit(axp_dev_handle, wr_ldo0, 2, I2C_MASTER_TIMEOUT_MS);
    uint8_t wr_ldo1[2] = {0x91, 0x00}; // DLDO2 OFF
    (void)i2c_master_transmit(axp_dev_handle, wr_ldo1, 2, I2C_MASTER_TIMEOUT_MS);

    // 7. Init GPIO18 (Coin Vibration Motor NPN driver) LOW + one-shot shutoff timer
    gpio_config_t vib_io = {
        .pin_bit_mask = (1ULL << BSP_VIB_MOTOR_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&vib_io);
    gpio_set_level(BSP_VIB_MOTOR_GPIO, 0);
    if (!s_vib_off_timer) {
        esp_timer_create_args_t t_args = {
            .callback = vib_off_timer_cb,
            .arg = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "vib_off",
            .skip_unhandled_events = true,
        };
        (void)esp_timer_create(&t_args, &s_vib_off_timer);
    }

    ESP_LOGI(TAG, "AXP2101 PMIC optimized (3 unused DCDCs + 7 unused LDOs shut down; 300mA/4.2V chg; Haptic ready)");
    bsp_pmu_log_diagnostics();
    return ESP_OK;
}

static volatile int s_cached_battery_percent = -1;
static volatile bool s_cached_battery_charging = false;
static volatile bool s_cached_vbus_present = false;
static int64_t s_last_battery_read_us = -10000000LL;
static int64_t s_last_charging_read_us = -10000000LL;
static int64_t s_last_voltage_read_us = -10000000LL;
static int s_cached_voltage_mv = 3850;
static int64_t s_last_vbus_read_us = -10000000LL;
static int s_cached_vbus_mv = 0;

int bsp_battery_get_percent(void) {
    if (!axp_dev_handle) return 100;
    
    int64_t now = esp_timer_get_time();
    if (s_cached_battery_percent >= 0 && (now - s_last_battery_read_us < 3000000LL)) {
        return s_cached_battery_percent;
    }

    uint8_t reg = 0xA4; // Battery percentage register
    uint8_t percent = 0;
    esp_err_t ret = i2c_master_transmit_receive(axp_dev_handle, &reg, 1, &percent, 1, 20);
    if (ret == ESP_OK) {
        s_last_battery_read_us = now;
        int val = (percent <= 100) ? (int)percent : (int)(percent & 0x7F);
        if (val >= 0 && val <= 100) {
            s_cached_battery_percent = val;
            return val;
        }
    }
    return (s_cached_battery_percent >= 0) ? s_cached_battery_percent : 100;
}

int bsp_battery_get_voltage_mv(void) {
    if (!axp_dev_handle) return 3850;
    int64_t now = esp_timer_get_time();
    if (now - s_last_voltage_read_us < 3000000LL) {
        return s_cached_voltage_mv;
    }
    uint8_t reg = 0x34; // Battery voltage register (0x34 high byte, 0x35 low byte)
    uint8_t buf[2] = {0};
    esp_err_t ret = i2c_master_transmit_receive(axp_dev_handle, &reg, 1, buf, 2, 20);
    if (ret == ESP_OK) {
        s_last_voltage_read_us = now;
        int v_mv = ((buf[0] & 0x3F) << 8) | buf[1];
        if (v_mv >= 2500 && v_mv <= 4450) {
            s_cached_voltage_mv = v_mv;
            return v_mv;
        }
    }
    return s_cached_voltage_mv;
}

int bsp_battery_get_vbus_mv(void) {
    if (!axp_dev_handle) return 0;
    int64_t now = esp_timer_get_time();
    if (now - s_last_vbus_read_us < 3000000LL) {
        return s_cached_vbus_mv;
    }
    uint8_t reg = 0x38; // VBUS voltage register (0x38 high, 0x39 low)
    uint8_t buf[2] = {0};
    if (i2c_master_transmit_receive(axp_dev_handle, &reg, 1, buf, 2, 20) == ESP_OK) {
        s_last_vbus_read_us = now;
        int v_mv = ((buf[0] & 0x3F) << 8) | buf[1];
        s_cached_vbus_mv = (v_mv >= 3500 && v_mv <= 6500) ? v_mv : 0;
    }
    return s_cached_vbus_mv;
}

bool bsp_battery_is_charging(void) {
    if (!axp_dev_handle) return false;
    
    int64_t now = esp_timer_get_time();
    if (now - s_last_charging_read_us < 3000000LL) {
        return s_cached_battery_charging;
    }

    uint8_t reg = 0x00;
    uint8_t st[2] = {0};
    esp_err_t ret = i2c_master_transmit_receive(axp_dev_handle, &reg, 1, st, 2, 20);
    if (ret == ESP_OK) {
        s_last_charging_read_us = now;
        s_cached_vbus_present = (st[0] & (1 << 5)) != 0;
        // AXP2101 Reg 0x01 Bits [6:5]: 01 = Charging
        s_cached_battery_charging = (((st[1] >> 5) & 0x03) == 0x01);
        return s_cached_battery_charging;
    }
    return s_cached_battery_charging;
}

bool bsp_battery_is_vbus_present(void) {
    (void)bsp_battery_is_charging(); // Refreshes st[0] & st[1] cache
    return s_cached_vbus_present;
}

esp_err_t bsp_pmu_set_amoled_power(bool enable) {
    if (!axp_dev_handle) return ESP_ERR_INVALID_STATE;
    
    uint8_t reg = 0x90; // LDOS ON/OFF control 0 (ALDO1-4, BLDO1-2, CPUSLDO, DLDO1)
    uint8_t val = 0;
    esp_err_t ret = i2c_master_transmit_receive(axp_dev_handle, &reg, 1, &val, 1, I2C_MASTER_TIMEOUT_MS);
    if (ret != ESP_OK) return ret;
    
    if (enable) {
        val |= (1 << 1); // Bit 1: ALDO2 Enable (DSI_PWR_EN)
    } else {
        val &= ~(1 << 1); // Bit 1: ALDO2 Disable (DSI_PWR_EN)
    }
    
    uint8_t write_buf[2] = {0x90, val};
    ret = i2c_master_transmit(axp_dev_handle, write_buf, 2, I2C_MASTER_TIMEOUT_MS);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "AXP2101 ALDO2 (AMOLED DSI_PWR_EN): %s", enable ? "ON" : "OFF");
    }
    return ret;
}

esp_err_t bsp_extra_init(void)
{
    esp_err_t ret;

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
    }

    ret = wifi_manager_pre_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "wifi_manager_pre_init failed");
    }

    ret = bsp_battery_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Battery AXP2101 init failed");
    }

    return ESP_OK;
}
