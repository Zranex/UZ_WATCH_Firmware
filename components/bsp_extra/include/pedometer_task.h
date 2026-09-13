#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize and start the pedometer task
 * @return ESP_OK on success
 */
esp_err_t pedometer_task_init(void);

/**
 * @brief Get the current step count
 * @return Number of steps
 */
uint32_t pedometer_get_steps(void);

/**
 * @brief Reset the step count to zero
 */
void pedometer_reset_steps(void);

#include "qmi8658.h"

/**
 * @brief Get the latest accelerometer reading without an extra I2C transaction
 */
esp_err_t pedometer_get_latest_acc(qmi8658_acc_t *acc);

#ifdef __cplusplus
}
#endif
