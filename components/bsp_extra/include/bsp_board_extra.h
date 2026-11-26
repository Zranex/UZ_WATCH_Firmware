#pragma once

#include <sys/cdefs.h>
#include <stdbool.h>
#include "esp_codec_dev.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t bsp_extra_init(void);

// Battery Telemetry & Power Rails
esp_err_t bsp_battery_init(void);
int bsp_battery_get_percent(void);
int bsp_battery_get_voltage_mv(void);
int bsp_battery_get_vbus_mv(void);
bool bsp_battery_is_charging(void);
bool bsp_battery_is_vbus_present(void);
void bsp_pmu_log_diagnostics(void);
void bsp_vibrate_pulse(uint32_t duration_ms);
esp_err_t bsp_pmu_set_amoled_power(bool enable);

// Shared Audio Codec/I2S Bus Mutex (prevents TARS/Music/Recorder/Alarm/SchoolBell codec races)
bool bsp_audio_lock(uint32_t timeout_ms);
void bsp_audio_unlock(void);

#ifdef __cplusplus
}
#endif
