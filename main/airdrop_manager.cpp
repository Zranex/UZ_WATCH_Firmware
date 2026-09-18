#include "airdrop_manager.hpp"
#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include <sys/stat.h>
#include <dirent.h>
#include <string.h>
#include <sys/param.h>
#include <unistd.h>
#include "esp_heap_caps.h"

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

    // RAM Dostu: Gelen devasa dosyayi chunk'lar halinde SD Karta yazar (PSRAM tercihli)
    char *chunk = (char *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!chunk) {
        chunk = (char *)malloc(4096);
    }
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

static const char s_airdrop_index_html[] = 
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>UZ WATCH AirDrop</title><style>"
"body{font-family:-apple-system,BlinkMacSystemFont,sans-serif;background:#121212;color:#fff;margin:0;padding:20px;text-align:center}"
"h1{color:#00e676;margin-bottom:5px}p{color:#aaa;font-size:14px}"
".card{background:#1e1e1e;border-radius:12px;padding:20px;margin:15px auto;max-width:400px;border:1px solid #333}"
"input[type=file]{margin:15px 0;color:#ccc;width:100%}"
"button{background:#00e676;color:#000;border:none;padding:12px 24px;border-radius:8px;font-size:16px;font-weight:bold;cursor:pointer;width:100%}"
"button:hover{background:#00c853}#status{margin-top:15px;font-weight:bold;color:#00e676}"
".files{text-align:left;margin-top:20px}"
".file-item{background:#252525;padding:10px 15px;border-radius:8px;margin:8px 0;display:flex;justify-content:space-between;align-items:center}"
".file-name{word-break:break-all;font-size:14px}a.dl{color:#00e676;text-decoration:none;font-weight:bold;margin-left:10px}"
"</style></head><body><h1>UZ WATCH AirDrop</h1><p>Kablosuz Dosya Transfer Portali</p>"
"<div class='card'><h3>Saate Dosya Yukle</h3><input type='file' id='fileInput'><br>"
"<button onclick='uploadFile()'>Saate Gonder</button><div id='status'></div></div>"
"<div class='card files'><h3>Saatteki Dosyalar (/AirDrop)</h3><div id='fileList'>Yukleniyor...</div></div>"
"<script>"
"function loadFiles(){fetch('/list').then(r=>r.json()).then(files=>{"
"let h='';if(files.length===0){h='<p style=\"color:#666\">Hic dosya yok</p>';}"
"else{files.forEach(f=>{h+=`<div class='file-item'><span class='file-name'>${f.name}</span><a class='dl' href='/download?filename=${encodeURIComponent(f.name)}' download>Indir</a></div>`;});}"
"document.getElementById('fileList').innerHTML=h;}).catch(e=>{document.getElementById('fileList').innerHTML='Dosya listesi alinamadi';});}"
"function uploadFile(){let f=document.getElementById('fileInput').files[0];"
"if(!f){alert('Lutfen bir dosya secin!');return;}"
"let s=document.getElementById('status');s.innerText='Yukleniyor: '+f.name+'...';"
"fetch('/upload?filename='+encodeURIComponent(f.name),{method:'POST',body:f})"
".then(r=>{if(r.ok){s.innerText='Basariyla yuklendi!';loadFiles();}else{s.innerText='Hata olustu!';}})"
".catch(e=>{s.innerText='Yukleme hatasi!';});}"
"loadFiles();</script></body></html>";

// 3. Download file (GET /download?filename=abc.txt)
static esp_err_t download_get_handler(httpd_req_t *req) {
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

    FILE *fd = fopen(filepath, "r");
    if (!fd) {
        ESP_LOGE(TAG, "Failed to open file for download: %s", filepath);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/octet-stream");
    char *chunk = (char *)malloc(2048);
    if (!chunk) {
        fclose(fd);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    size_t read_bytes;
    while ((read_bytes = fread(chunk, 1, 2048, fd)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, read_bytes) != ESP_OK) {
            fclose(fd);
            free(chunk);
            return ESP_FAIL;
        }
    }
    fclose(fd);
    free(chunk);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// 4. Index page (GET /)
static esp_err_t index_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, s_airdrop_index_html);
    return ESP_OK;
}

// URIs
static const httpd_uri_t uri_root = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = index_get_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_index = {
    .uri       = "/index.html",
    .method    = HTTP_GET,
    .handler   = index_get_handler,
    .user_ctx  = NULL
};

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

static const httpd_uri_t uri_download = {
    .uri       = "/download",
    .method    = HTTP_GET,
    .handler   = download_get_handler,
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
        httpd_register_uri_handler(server, &uri_root);
        httpd_register_uri_handler(server, &uri_index);
        httpd_register_uri_handler(server, &uri_list);
        httpd_register_uri_handler(server, &uri_upload);
        httpd_register_uri_handler(server, &uri_download);
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






