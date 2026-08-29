#include "airdrop_manager.hpp"
#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include <sys/stat.h>
#include <dirent.h>
#include <string.h>
#include <sys/param.h>
#include <unistd.h>

static const char *TAG = "AirDrop";
bool AirDropManager::_is_sd_mounted = false;
void* AirDropManager::_server_handle = nullptr;

// Base path for AirDrop files
#define AIRDROP_BASE_PATH BSP_SD_MOUNT_POINT "/AirDrop"

/* HTTP Handlers */

// 1. List files (GET /list)
static esp_err_t list_get_handler(httpd_req_t *req) {
    DIR *dir = opendir(AIRDROP_BASE_PATH);
    if (!dir) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "[");
    
    bool first = true;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_DIR) continue;
        
        char json[512];
        snprintf(json, sizeof(json), "%s{\"name\":\"%s\"}", first ? "" : ",", entry->d_name);
        httpd_resp_sendstr_chunk(req, json);
        first = false;
    }
    closedir(dir);
    
    httpd_resp_sendstr_chunk(req, "]");
    httpd_resp_sendstr_chunk(req, NULL); // Finish chunked response
    return ESP_OK;
}

// 2. Upload file (POST /upload?filename=abc.txt)
static esp_err_t upload_post_handler(httpd_req_t *req) {
    char filepath[256];
    size_t buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        char *buf = (char *)malloc(buf_len);
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            char param[64];
            if (httpd_query_key_value(buf, "filename", param, sizeof(param)) == ESP_OK) {
                snprintf(filepath, sizeof(filepath), "%s/%s", AIRDROP_BASE_PATH, param);
            } else {
                free(buf);
                httpd_resp_send_404(req);
                return ESP_FAIL;
            }
        }
        free(buf);
    } else {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    FILE *fd = fopen(filepath, "w");
    if (!fd) {
        ESP_LOGE(TAG, "Failed to create file : %s", filepath);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Receiving file : %s...", filepath);

    // RAM Dostu: Gelen devasa dosyayi chunk'lar halinde SD Karta yazar
    char *chunk = (char *)malloc(4096); // 8KB buffer
    if (!chunk) {
        fclose(fd);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int received;
    int remaining = req->content_len;
    while (remaining > 0) {
        if ((received = httpd_req_recv(req, chunk, MIN(remaining, 4096))) <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                continue; // Retry receiving if timeout occurred
            }
            // Error occurred
            fclose(fd);
            free(chunk);
            unlink(filepath); // Silik ve bozuk dosyayi sil
            ESP_LOGE(TAG, "File reception failed!");
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        fwrite(chunk, 1, received, fd);
        remaining -= received;
        
        // Watchdog timeout yasamamak icin kucuk bir yield
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    
    fclose(fd);
    free(chunk);
    ESP_LOGI(TAG, "File reception complete");

    httpd_resp_sendstr(req, "File uploaded successfully");
    return ESP_OK;
}

// URIs
static const httpd_uri_t uri_list = {
    .uri       = "/list",
    .method    = HTTP_GET,
    .handler   = list_get_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_upload = {
    .uri       = "/upload",
    .method    = HTTP_POST,
    .handler   = upload_post_handler,
    .user_ctx  = NULL
};


esp_err_t AirDropManager::init() {
    if (_is_sd_mounted) return ESP_OK;

    ESP_LOGI(TAG, "Checking SD Card mount status...");
    DIR* dir = opendir(BSP_SD_MOUNT_POINT);
    if (dir) {
        closedir(dir);
        _is_sd_mounted = true;
        ESP_LOGI(TAG, "SD Card already mounted by system.");
    } else {
        ESP_LOGI(TAG, "Mounting SD Card explicitly...");
        esp_err_t ret = bsp_sdcard_mount();
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "Failed to mount SD card (err: %s)", esp_err_to_name(ret));
            return ret;
        }
        _is_sd_mounted = true;
    }

    // Create /AirDrop directory if it doesn't exist
    struct stat st = {0};
    if (stat(AIRDROP_BASE_PATH, &st) == -1) {
        mkdir(AIRDROP_BASE_PATH, 0777);
        ESP_LOGI(TAG, "Created %s directory", AIRDROP_BASE_PATH);
    }

    return ESP_OK;
}

esp_err_t AirDropManager::start_server() {
    if (!_is_sd_mounted) {
        ESP_LOGW(TAG, "SD Card not mounted. Cannot start AirDrop server.");
        return ESP_FAIL;
    }
    if (_server_handle) return ESP_OK; // Already running

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    // RAM Stabilizasyonu: Ayni anda en fazla 2 baglanti
    config.max_open_sockets = 2;
    // Buyuk dosyalar icin HTTPD core stack size artirildi
    config.stack_size = 6144; 
    
    // Gelen dosya buffer limiti
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 5;

    ESP_LOGI(TAG, "Starting AirDrop HTTP Server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &uri_list);
        httpd_register_uri_handler(server, &uri_upload);
        _server_handle = (void*)server;
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Error starting server!");
    return ESP_FAIL;
}

void AirDropManager::stop_server() {
    if (_server_handle) {
        httpd_stop((httpd_handle_t)_server_handle);
        _server_handle = nullptr;
        ESP_LOGI(TAG, "AirDrop Server stopped.");
    }
}






