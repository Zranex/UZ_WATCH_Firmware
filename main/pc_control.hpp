#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Direct Watch -> PC Ethernet Wake-on-LAN Magic Packet (works even when PC & Python agent are OFF!)
bool pc_control_wake_lan(void);

// Direct Watch -> PC HTTP POST to UZ_PC_Agent (auto-discovers PC IP via UDP 5001 if DHCP changed!)
bool pc_control_send(const char* cmd);

// Direct Watch -> PC HTTP GET /stats (fetches live CPU %, RAM %, Free Disk summary)
bool pc_control_get_stats(char* out_summary, size_t max_len);

// Non-blocking async dispatch safe to call from LVGL UI buttons (never freezes taskLVGL!)
void pc_control_send_async(const char* cmd);

// Direct Watch -> ESPHome Salon Light (192.168.1.102) HTTP POST
bool pc_control_esphome(const char* path);

// Returns currently cached PC IP (default 192.168.1.193)
const char* pc_control_get_cached_ip(void);

#ifdef __cplusplus
}
#endif
