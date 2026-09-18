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
bool bsp_battery_is_charging(void);
esp_err_t bsp_pmu_set_amoled_power(bool enable);

#ifdef __cplusplus
}
#endif
