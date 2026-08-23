#pragma once

#include "esp_err.h"

class AirDropManager {
public:
    // Mounts SD card and creates /AirDrop directory
    static esp_err_t init();
    
    // Starts the local HTTP file server
    static esp_err_t start_server();
    
    // Stops the HTTP server
    static void stop_server();

private:
    static bool _is_sd_mounted;
    static void* _server_handle; // httpd_handle_t
};
