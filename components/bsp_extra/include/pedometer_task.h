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

#ifdef __cplusplus
}
#endif
