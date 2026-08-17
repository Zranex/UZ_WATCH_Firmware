#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the BLE Manager and start advertising as UZ WATCH
 */
esp_err_t ble_manager_init(void);

#ifdef __cplusplus
}
#endif
