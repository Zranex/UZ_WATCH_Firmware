#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Queue a TV command asynchronously (safe to call from LVGL UI buttons — never blocks UI!)
// Keeps TLS session to Tivibu AndroidTV (192.168.1.159:6466) warm for 25s -> ~15ms instant remote clicks!
void tv_control_send_async(const char* cmd);

// Send a TV command synchronously (called from TARS worker task; auto-connects if idle)
bool tv_control_send(const char* cmd);

// Gracefully close active warm TV TLS session (e.g. before AirDrop SoftAP or Wi-Fi stop)
void tv_control_disconnect(void);

// Returns currently configured TV IP (default 192.168.1.159)
const char* tv_control_get_ip(void);

#ifdef __cplusplus
}
#endif
