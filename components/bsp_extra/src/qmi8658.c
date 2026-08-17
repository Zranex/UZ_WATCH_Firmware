#include "qmi8658.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "bsp_board_extra.h"
#include "bsp/esp-bsp.h"

static const char *TAG = "QMI8658";
static i2c_master_dev_handle_t qmi_dev_handle = NULL;

#define QMI8658_ADDR 0x6B // Default I2C address for QMI8658 (SA0=1)
#define QMI8658_WHO_AM_I 0x00
#define QMI8658_CTRL1 0x02
#define QMI8658_CTRL2 0x03
#define QMI8658_CTRL3 0x04
#define QMI8658_CTRL7 0x08
#define QMI8658_AX_L 0x35

static esp_err_t qmi_write_reg(uint8_t reg, uint8_t val) {
    uint8_t data[2] = {reg, val};
    return i2c_master_transmit(qmi_dev_handle, data, 2, -1);
}

static esp_err_t qmi_read_regs(uint8_t reg, uint8_t *data, size_t len) {
    return i2c_master_transmit_receive(qmi_dev_handle, &reg, 1, data, len, -1);
}

esp_err_t qmi8658_init(void) {
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) return ESP_FAIL;

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = QMI8658_ADDR,
        .scl_speed_hz = 400000,
    };

    if (i2c_master_bus_add_device(bus, &dev_cfg, &qmi_dev_handle) != ESP_OK) {
        // Try alternate address
        dev_cfg.device_address = 0x6A;
        if (i2c_master_bus_add_device(bus, &dev_cfg, &qmi_dev_handle) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to add I2C device");
            return ESP_FAIL;
        }
    }

    uint8_t whoami = 0;
    qmi_read_regs(QMI8658_WHO_AM_I, &whoami, 1);
    ESP_LOGI(TAG, "QMI8658 WHO_AM_I: 0x%02X", whoami);
    
    if (whoami != 0x05) {
        ESP_LOGE(TAG, "QMI8658 not found!");
        return ESP_FAIL;
    }

    // Configure QMI8658
    qmi_write_reg(QMI8658_CTRL1, 0x40); // Address Auto-Increment
    qmi_write_reg(QMI8658_CTRL2, 0x01); // Accel Enable, 2g, 125Hz
    qmi_write_reg(QMI8658_CTRL3, 0x01); // Gyro Enable, 125dps, 125Hz
    qmi_write_reg(QMI8658_CTRL7, 0x03); // Enable Accel & Gyro
    
    ESP_LOGI(TAG, "QMI8658 initialized successfully");
    return ESP_OK;
}

esp_err_t qmi8658_read_acc(qmi8658_acc_t *acc) {
    if (!qmi_dev_handle) return ESP_FAIL;
    uint8_t data[6];
    if (qmi_read_regs(QMI8658_AX_L, data, 6) == ESP_OK) {
        int16_t x = (int16_t)((data[1] << 8) | data[0]);
        int16_t y = (int16_t)((data[3] << 8) | data[2]);
        int16_t z = (int16_t)((data[5] << 8) | data[4]);
        
        // 2g range -> 16384 LSB/g
        acc->x = (float)x / 16384.0f;
        acc->y = (float)y / 16384.0f;
        acc->z = (float)z / 16384.0f;
        return ESP_OK;
    }
    return ESP_FAIL;
}

esp_err_t qmi8658_read_gyro(qmi8658_gyro_t *gyro) {
    if (!qmi_dev_handle) return ESP_FAIL;
    uint8_t data[6];
    if (qmi_read_regs(0x3B, data, 6) == ESP_OK) { // Gyro data starts at 0x3B
        int16_t x = (int16_t)((data[1] << 8) | data[0]);
        int16_t y = (int16_t)((data[3] << 8) | data[2]);
        int16_t z = (int16_t)((data[5] << 8) | data[4]);
        
        // 125dps range -> 262.144 LSB/dps
        gyro->x = (float)x / 262.144f;
        gyro->y = (float)y / 262.144f;
        gyro->z = (float)z / 262.144f;
        return ESP_OK;
    }
    return ESP_FAIL;
}
