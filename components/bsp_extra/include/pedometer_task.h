#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize and start the pedometer task (restores today's steps from NVS)
 * @return ESP_OK on success
 */
esp_err_t pedometer_task_init(void);

/**
 * @brief Get the current step count
 * @return Number of steps
 */
uint32_t pedometer_get_steps(void);

/**
 * @brief Reset the step count to zero (and persist immediately)
 */
void pedometer_reset_steps(void);

/**
 * @brief Flush current step count to NVS immediately if changed
 */
void pedometer_save_now(void);

/**
 * @brief Format recent daily step history (yesterday + last 7 days from SD/NVS) for TARS AI context
 */
void pedometer_format_recent_history(char* out, size_t max_len);

#include "qmi8658.h"

/**
 * @brief Get the latest accelerometer reading without an extra I2C transaction
 */
esp_err_t pedometer_get_latest_acc(qmi8658_acc_t *acc);

/**
 * @brief Set pedometer sampling rate to low-power mode (150ms/400ms instead of 100ms)
 */
void pedometer_set_low_power(bool enable);

typedef void (*pedometer_acc_cb_t)(const qmi8658_acc_t *acc);
void pedometer_register_acc_callback(pedometer_acc_cb_t cb);

#ifdef __cplusplus
}
#endif
