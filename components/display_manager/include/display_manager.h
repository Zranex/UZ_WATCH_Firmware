#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

void display_manager_init(void);
void display_manager_turn_on(void);
void display_manager_turn_off(void);
bool display_manager_is_on(void);
void display_manager_reset_timer(void);

void display_manager_set_timeout(uint32_t timeout_ms);
uint32_t display_manager_get_timeout(void);

void display_manager_set_wake_cb(void (*cb)(void));

void display_manager_set_brightness(uint8_t brightness);
uint8_t display_manager_get_brightness(void);

// Early PM setup: create and acquire a NO_LIGHT_SLEEP lock so the
// system won’t enter light-sleep during boot/UI init. Safe to call multiple times.
void display_manager_pm_early_init(void);

#ifdef __cplusplus
}
#endif
