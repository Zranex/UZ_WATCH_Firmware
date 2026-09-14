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

/**
 * @brief Send a media control command to the connected BLE device (Notify)
 */
void ble_manager_send_media_command(const char* command);

/**
 * @brief Check if BLE is enabled
 */
bool ble_manager_is_active(void);

/**
 * @brief Enable or disable BLE (turns off advertising & radio to save power)
 */
void ble_manager_set_active(bool enable);

#ifdef __cplusplus
}
#endif
