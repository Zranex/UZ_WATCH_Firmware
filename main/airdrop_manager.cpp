#include "airdrop_manager.hpp"
#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include <sys/stat.h>
#include <dirent.h>
#include <string.h>
#include <sys/param.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include "esp_heap_caps.h"

static const char *TAG = "AirDrop";
bool AirDropManager::_is_sd_mounted = false;
void* AirDropManager::_server_handle = nullptr;

// Base path for AirDrop files
#define AIRDROP_BASE_PATH BSP_SD_MOUNT_POINT "/AirDrop"

/* URL Decoding Helper */
static void url_decode(char *dst, const char *src, size_t max_len) {
    size_t i = 0, o = 0;
    while (src[i] && o + 1 < max_len) {
        if (src[i] == '%' && src[i+1] && src[i+2]) {
            int h1 = src[i+1];
            int h2 = src[i+2];
            int v1 = (h1 >= '0' && h1 <= '9') ? (h1 - '0') :
                     (h1 >= 'a' && h1 <= 'f') ? (h1 - 'a' + 10) :
                     (h1 >= 'A' && h1 <= 'F') ? (h1 - 'A' + 10) : -1;
            int v2 = (h2 >= '0' && h2 <= '9') ? (h2 - '0') :
                     (h2 >= 'a' && h2 <= 'f') ? (h2 - 'a' + 10) :
                     (h2 >= 'A' && h2 <= 'F') ? (h2 - 'A' + 10) : -1;
            if (v1 >= 0 && v2 >= 0) {
                dst[o++] = (char)((v1 << 4) | v2);
                i += 3;
                continue;
            }
        } else if (src[i] == '+') {
            dst[o++] = ' ';
            i++;
            continue;
        }
        dst[o++] = src[i++];
    }
    dst[o] = '\0';
}

/* Filename Sanitizer for FATFS (Maps Turkish & special chars to safe ASCII) */
static void sanitize_filename(char *dst, const char *src, size_t max_len) {
    size_t i = 0, o = 0;
    while (src[i] && o + 1 < max_len) {
        unsigned char c = (unsigned char)src[i];
        
        // Handle Turkish UTF-8 multi-byte characters
        if (c == 0xC3) { // ç, ö, ü
            unsigned char c2 = (unsigned char)src[i+1];
            if (c2 == 0xA7 || c2 == 0x87) { dst[o++] = 'c'; i += 2; continue; } // ç / Ç
            if (c2 == 0xB6 || c2 == 0x96) { dst[o++] = 'o'; i += 2; continue; } // ö / Ö
            if (c2 == 0xBC || c2 == 0x9C) { dst[o++] = 'u'; i += 2; continue; } // ü / Ü
        } else if (c == 0xC4) { // ğ, ı, İ
            unsigned char c2 = (unsigned char)src[i+1];
            if (c2 == 0x9F || c2 == 0x9E) { dst[o++] = 'g'; i += 2; continue; } // ğ / Ğ
            if (c2 == 0xB1) { dst[o++] = 'i'; i += 2; continue; } // ı
            if (c2 == 0xB0) { dst[o++] = 'I'; i += 2; continue; } // İ
        } else if (c == 0xC5) { // ş
            unsigned char c2 = (unsigned char)src[i+1];
            if (c2 == 0x9F || c2 == 0x9E) { dst[o++] = 's'; i += 2; continue; } // ş / Ş
        }

        // Invalid FAT characters: / \ : * ? " < > | or control characters (< 32)
        if (c < 32 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|' || c > 126) {
            dst[o++] = '_';
        } else {
            dst[o++] = (char)c;
        }
        i++;
    }
    dst[o] = '\0';

    // Fallback if filename became empty
    if (strlen(dst) == 0) {
        strncpy(dst, "upload_file.bin", max_len - 1);
        dst[max_len - 1] = '\0';
    }
}

/* HTTP Handlers */

// 1. List files (GET /list)
static esp_err_t list_get_handler(httpd_req_t *req) {
    const char *base = AIRDROP_BASE_PATH;
    DIR *dir = opendir(base);
    if (!dir) {
        base = BSP_SD_MOUNT_POINT;
        dir = opendir(base);
    }
    if (!dir) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD Kart okunamadi");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "[");
    
    bool first = true;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_DIR) continue;
        if (entry->d_name[0] == '.') continue;
        
        char fullpath[384];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", base, entry->d_name);
        struct stat st = {0};
        int64_t fsize = 0;
        if (stat(fullpath, &st) == 0) {
            fsize = (int64_t)st.st_size;
        }

        char json[384];
        snprintf(json, sizeof(json), "%s{\"name\":\"%s\",\"size\":%lld}", first ? "" : ",", entry->d_name, (long long)fsize);
        httpd_resp_sendstr_chunk(req, json);
        first = false;
    }
    closedir(dir);
    
    httpd_resp_sendstr_chunk(req, "]");
    httpd_resp_sendstr_chunk(req, NULL); // Finish chunked response
    return ESP_OK;
}

// 2. Upload file (POST /upload?filename=abc.jpg)
static esp_err_t upload_post_handler(httpd_req_t *req) {
    char raw_filename[256] = {0};
    size_t buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        char *buf = (char *)malloc(buf_len);
        if (buf && httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            httpd_query_key_value(buf, "filename", raw_filename, sizeof(raw_filename));
        }
        if (buf) free(buf);
    }

    // If filename wasn't found in query param, generate timestamped fallback
    if (strlen(raw_filename) == 0) {
        snprintf(raw_filename, sizeof(raw_filename), "photo_%lu.jpg", (unsigned long)time(NULL));
    }

    char decoded_filename[256];
    url_decode(decoded_filename, raw_filename, sizeof(decoded_filename));

    char clean_filename[256];
    sanitize_filename(clean_filename, decoded_filename, sizeof(clean_filename));

    ESP_LOGI(TAG, "Upload request: raw='%s', clean='%s', size=%d bytes", 
             raw_filename, clean_filename, req->content_len);

    // Ensure AirDrop directory exists
    struct stat st = {0};
    if (stat(AIRDROP_BASE_PATH, &st) == -1) {
        mkdir(AIRDROP_BASE_PATH, 0777);
    }

    char filepath[320];
    snprintf(filepath, sizeof(filepath), "%s/%s", AIRDROP_BASE_PATH, clean_filename);

    FILE *fd = fopen(filepath, "wb");
    if (!fd) {
        ESP_LOGE(TAG, "Failed to create %s (errno %d: %s). Trying root SD...", filepath, errno, strerror(errno));
        // Fallback: Try root SD card folder
        snprintf(filepath, sizeof(filepath), "%s/%s", BSP_SD_MOUNT_POINT, clean_filename);
        fd = fopen(filepath, "wb");
        if (!fd) {
            ESP_LOGE(TAG, "Fallback also failed: %s (errno %d: %s)", filepath, errno, strerror(errno));
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD Karta dosya acilamadi");
            return ESP_FAIL;
        }
    }

    ESP_LOGI(TAG, "Receiving file to : %s...", filepath);

    // PSRAM buffer prefered for high-speed SD transfer
    char *chunk = (char *)heap_caps_malloc(8192, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!chunk) {
        chunk = (char *)malloc(4096);
    }
    int chunk_size = chunk ? 8192 : 4096;
    if (!chunk) {
        fclose(fd);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Yetersiz RAM");
        return ESP_FAIL;
    }

    int received;
    int remaining = req->content_len;
    int timeout_retries = 0;

    while (remaining > 0) {
        int to_recv = MIN(remaining, chunk_size);
        received = httpd_req_recv(req, chunk, to_recv);
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                if (++timeout_retries < 20) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                    continue; // Retry receiving
                }
            }
            // Error occurred
            fclose(fd);
            free(chunk);
            unlink(filepath); // Delete incomplete file
            ESP_LOGE(TAG, "File reception failed! (err: %d)", received);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Veri aktarimi kesildi");
            return ESP_FAIL;
        }
        timeout_retries = 0;
        fwrite(chunk, 1, received, fd);
        remaining -= received;
        
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    
    fflush(fd);
    fclose(fd);
    free(chunk);
    ESP_LOGI(TAG, "File reception complete: %s (%d bytes)", filepath, req->content_len);

    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static const char s_airdrop_index_html[] = 
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>UZ WATCH AirDrop</title><style>"
"body{font-family:-apple-system,BlinkMacSystemFont,sans-serif;background:#121212;color:#fff;margin:0;padding:20px;text-align:center}"
"h1{color:#00e676;margin-bottom:5px;font-size:24px}p{color:#aaa;font-size:14px;margin-top:0}"
".card{background:#1e1e1e;border-radius:14px;padding:20px;margin:15px auto;max-width:440px;border:1px solid #333;box-sizing:border-box}"
"input[type=file]{margin:15px 0;color:#ccc;width:100%;box-sizing:border-box}"
"button.btn-send{background:#00e676;color:#000;border:none;padding:12px 24px;border-radius:8px;font-size:16px;font-weight:bold;cursor:pointer;width:100%}"
"button.btn-send:hover{background:#00c853}"
"#status{margin-top:15px;font-weight:bold;color:#00e676;font-size:14px;word-break:break-all}"
".progress-bar{width:100%;height:8px;background:#333;border-radius:4px;overflow:hidden;margin-top:12px;display:none}"
".progress-fill{height:100%;background:#00e676;width:0%;transition:width 0.2s}"
".files{text-align:left;margin-top:20px}"
".file-item{background:#252525;padding:12px 14px;border-radius:10px;margin:10px 0;display:flex;justify-content:space-between;align-items:center;border:1px solid #333}"
".file-info{display:flex;flex-direction:column;overflow:hidden;margin-right:10px;flex:1}"
".file-name{word-break:break-all;font-size:14px;font-weight:500}"
".file-size{font-size:11px;color:#888;margin-top:3px}"
".file-actions{display:flex;align-items:center;gap:6px;flex-shrink:0}"
".btn-view{background:#00e676;color:#000;border:none;padding:6px 12px;border-radius:6px;font-size:12px;font-weight:bold;cursor:pointer;white-space:nowrap}"
".btn-view:hover{background:#00c853}"
"a.dl{background:#333;color:#eee;text-decoration:none;font-weight:600;padding:6px 10px;border-radius:6px;font-size:12px;white-space:nowrap}"
"a.dl:hover{background:#444;color:#fff}"
".modal{display:none;position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,0.88);z-index:9999;align-items:center;justify-content:center;padding:12px;box-sizing:border-box}"
".modal-content{background:#1e1e1e;border-radius:16px;max-width:540px;width:100%;max-height:92vh;display:flex;flex-direction:column;border:1px solid #3a3a3a;overflow:hidden;box-shadow:0 12px 40px rgba(0,0,0,0.9)}"
".modal-header{display:flex;justify-content:space-between;align-items:center;padding:12px 16px;border-bottom:1px solid #2a2a2a}"
".modal-title{font-size:14px;font-weight:600;color:#00e676;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;max-width:80%}"
".btn-close{background:none;border:none;color:#aaa;font-size:26px;line-height:1;cursor:pointer;padding:0;width:32px;height:32px}"
".btn-close:hover{color:#fff}"
".media-box{flex:1;display:flex;align-items:center;justify-content:center;padding:12px;overflow:auto;background:#0d0d0d}"
".media-box img{max-width:100%;max-height:68vh;object-fit:contain;border-radius:8px}"
".media-box video{max-width:100%;max-height:68vh;border-radius:8px;background:#000;outline:none;width:100%}"
".modal-footer{padding:10px 16px;border-top:1px solid #2a2a2a;text-align:right}"
"</style></head><body>"
"<h1>UZ WATCH AirDrop</h1>"
"<p>Kablosuz Medya & Dosya Portali</p>"
"<div class='card'>"
"<h3>Saate Dosya / Medya Yukle</h3>"
"<input type='file' id='fileInput'><br>"
"<button class='btn-send' id='btnSend' onclick='uploadFile()'>Saate Gonder</button>"
"<div class='progress-bar' id='progBar'><div class='progress-fill' id='progFill'></div></div>"
"<div id='status'></div>"
"</div>"
"<div class='card files'>"
"<h3>Saatteki Dosyalar (/AirDrop)</h3>"
"<div id='fileList'>Yukleniyor...</div>"
"</div>"
"<div id='mediaModal' class='modal' onclick='closeModal(event)'>"
"<div class='modal-content' onclick='event.stopPropagation()'>"
"<div class='modal-header'>"
"<span id='modalTitle' class='modal-title'></span>"
"<button class='btn-close' onclick='closeModal()'>&times;</button>"
"</div>"
"<div id='mediaBox' class='media-box'></div>"
"<div class='modal-footer'>"
"<a id='modalDl' class='dl' href='#' download>Telefona Indir</a>"
"</div>"
"</div>"
"</div>"
"<script>"
"function fmtSize(b){"
"  if(!b||b===0) return '';"
"  let k=1024,s=['B','KB','MB','GB'],i=Math.floor(Math.log(b)/Math.log(k));"
"  return (b/Math.pow(k,i)).toFixed(1)+' '+s[i];"
"}"
"function isImg(n){ return /\\.(jpg|jpeg|png|gif|webp|bmp)$/i.test(n); }"
"function isVid(n){ return /\\.(mp4|webm|mov|mkv)$/i.test(n); }"
"function isAud(n){ return /\\.(mp3|wav|ogg|m4a|aac)$/i.test(n); }"
"function openMedia(name){"
"  let modal=document.getElementById('mediaModal');"
"  let title=document.getElementById('modalTitle');"
"  let box=document.getElementById('mediaBox');"
"  let dl=document.getElementById('modalDl');"
"  let enc=encodeURIComponent(name);"
"  let src='/view?filename='+enc;"
"  title.innerText=name;"
"  dl.href='/download?filename='+enc;"
"  dl.download=name;"
"  if(isImg(name)){"
"    box.innerHTML=\"<img src='\"+src+\"' alt='\"+name+\"'>\";"
"  }else if(isVid(name)){"
"    box.innerHTML=\"<video src='\"+src+\"' controls autoplay playsinline></video>\";"
"  }else if(isAud(name)){"
"    box.innerHTML=\"<div style='text-align:center;width:100%;padding:20px 0'><div style='font-size:48px;margin-bottom:12px'>🎵</div><audio src='\"+src+\"' controls autoplay style='width:90%'></audio></div>\";"
"  }else{"
"    window.open(src,'_blank');"
"    return;"
"  }"
"  modal.style.display='flex';"
"}"
"function closeModal(){"
"  let m=document.getElementById('mediaModal');"
"  let b=document.getElementById('mediaBox');"
"  m.style.display='none';"
"  b.innerHTML='';"
"}"
"document.addEventListener('keydown',function(e){if(e.key==='Escape')closeModal();});"
"function loadFiles(){"
"  fetch('/list').then(r=>r.json()).then(files=>{"
"    let h='';"
"    if(!files||files.length===0){ h='<p style=\"color:#666;text-align:center\">Hic dosya yok</p>'; }"
"    else{"
"      files.forEach(f=>{"
"        let icon='📄';"
"        let media=isImg(f.name)||isVid(f.name)||isAud(f.name);"
"        if(isImg(f.name)) icon='📷';"
"        else if(isVid(f.name)) icon='🎬';"
"        else if(isAud(f.name)) icon='🎵';"
"        let sz=f.size?fmtSize(f.size):'';"
"        let btnLabel=isVid(f.name)?'▶ Izle':(isAud(f.name)?'🎵 Dinle':'👁 Gor');"
"        let enc=encodeURIComponent(f.name);"
"        let safeName=f.name.replace(/\"/g,'&quot;').replace(/'/g,'&#39;');"
"        h+=\"<div class='file-item'>\"+"
"            \"<div class='file-info'>\"+"
"              \"<span class='file-name' style='\"+(media?\"cursor:pointer;color:#00e676\":\"\")+\"' onclick='\"+(media?\"openMedia(\\\"\"+safeName+\"\\\")\":\"\")+\"'>\"+icon+\" \"+safeName+\"</span>\"+"
"              (sz?\"<span class='file-size'>\"+sz+\"</span>\":\"\")+"
"            \"</div>\"+"
"            \"<div class='file-actions'>\"+"
"              (media?\"<button class='btn-view' onclick='openMedia(\\\"\"+safeName+\"\\\")'>\"+btnLabel+\"</button>\":\"\")+"
"              \"<a class='dl' href='/download?filename=\"+enc+\"' download>Indir</a>\"+"
"            \"</div>\"+"
"           \"</div>\";"
"      });"
"    }"
"    document.getElementById('fileList').innerHTML=h;"
"  }).catch(e=>{ document.getElementById('fileList').innerHTML='Dosya listesi alinamadi'; });"
"}"
"function uploadFile(){"
"  let fi=document.getElementById('fileInput');"
"  let f=fi.files[0];"
"  if(!f){ alert('Lutfen bir dosya secin!'); return; }"
"  let s=document.getElementById('status');"
"  let pBar=document.getElementById('progBar');"
"  let pFill=document.getElementById('progFill');"
"  let btn=document.getElementById('btnSend');"
"  btn.disabled=true;"
"  btn.style.opacity='0.6';"
"  pBar.style.display='block';"
"  pFill.style.width='0%';"
"  s.style.color='#00e676';"
"  s.innerText='Yukleme baslatiliyor...';"
"  let xhr=new XMLHttpRequest();"
"  xhr.open('POST','/upload?filename='+encodeURIComponent(f.name),true);"
"  xhr.upload.onprogress=function(e){"
"    if(e.lengthComputable){"
"      let pct=Math.round((e.loaded/e.total)*100);"
"      pFill.style.width=pct+'%';"
"      let lMb=(e.loaded/1048576).toFixed(1);"
"      let tMb=(e.total/1048576).toFixed(1);"
"      s.innerText='Yukleniyor: %'+pct+' ('+lMb+' / '+tMb+' MB)';"
"    }"
"  };"
"  xhr.onload=function(){"
"    btn.disabled=false;"
"    btn.style.opacity='1';"
"    if(xhr.status===200){"
"      pFill.style.width='100%';"
"      s.style.color='#00e676';"
"      s.innerText='Basariyla yuklendi: '+f.name;"
"      fi.value='';"
"      setTimeout(()=>{pBar.style.display='none';},2000);"
"      loadFiles();"
"    }else{"
"      s.style.color='#ff5252';"
"      s.innerText='Hata ('+xhr.status+'): '+(xhr.responseText||'Sunucu hatasi');"
"    }"
"  };"
"  xhr.onerror=function(){"
"    btn.disabled=false;"
"    btn.style.opacity='1';"
"    s.style.color='#ff5252';"
"    s.innerText='Baglanti hatasi olustu!';"
"  };"
"  xhr.send(f);"
"}"
"loadFiles();"
"</script></body></html>";

static const char* get_mime_type(const char *filename) {
    const char *ext = strrchr(filename, '.');
    if (!ext) return "application/octet-stream";
    if (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0) return "image/jpeg";
    if (strcasecmp(ext, ".png") == 0) return "image/png";
    if (strcasecmp(ext, ".gif") == 0) return "image/gif";
    if (strcasecmp(ext, ".webp") == 0) return "image/webp";
    if (strcasecmp(ext, ".bmp") == 0) return "image/bmp";
    if (strcasecmp(ext, ".svg") == 0) return "image/svg+xml";
    if (strcasecmp(ext, ".mp4") == 0) return "video/mp4";
    if (strcasecmp(ext, ".webm") == 0) return "video/webm";
    if (strcasecmp(ext, ".mov") == 0) return "video/quicktime";
    if (strcasecmp(ext, ".mkv") == 0) return "video/x-matroska";
    if (strcasecmp(ext, ".mp3") == 0) return "audio/mpeg";
    if (strcasecmp(ext, ".wav") == 0) return "audio/wav";
    if (strcasecmp(ext, ".ogg") == 0) return "audio/ogg";
    if (strcasecmp(ext, ".m4a") == 0) return "audio/mp4";
    if (strcasecmp(ext, ".aac") == 0) return "audio/aac";
    if (strcasecmp(ext, ".txt") == 0) return "text/plain; charset=utf-8";
    if (strcasecmp(ext, ".pdf") == 0) return "application/pdf";
    return "application/octet-stream";
}

// 3. Download file (GET /download?filename=abc.txt)
static esp_err_t download_get_handler(httpd_req_t *req) {
    char raw_filename[256] = {0};
    size_t buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        char *buf = (char *)malloc(buf_len);
        if (buf && httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            httpd_query_key_value(buf, "filename", raw_filename, sizeof(raw_filename));
        }
        if (buf) free(buf);
    }

    if (strlen(raw_filename) == 0) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    char decoded_filename[256];
    url_decode(decoded_filename, raw_filename, sizeof(decoded_filename));

    char filepath[320];
    snprintf(filepath, sizeof(filepath), "%s/%s", AIRDROP_BASE_PATH, decoded_filename);

    FILE *fd = fopen(filepath, "rb");
    if (!fd) {
        // Fallback: try root SD
        snprintf(filepath, sizeof(filepath), "%s/%s", BSP_SD_MOUNT_POINT, decoded_filename);
        fd = fopen(filepath, "rb");
    }
    if (!fd) {
        ESP_LOGE(TAG, "Failed to open file for download: %s", filepath);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/octet-stream");
    char *chunk = (char *)malloc(4096);
    if (!chunk) {
        fclose(fd);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    size_t read_bytes;
    while ((read_bytes = fread(chunk, 1, 4096, fd)) > 0) {
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

// 4. View / Stream file with HTTP 206 Range support (GET /view?filename=abc.mp4)
static esp_err_t view_get_handler(httpd_req_t *req) {
    char raw_filename[256] = {0};
    size_t buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        char *buf = (char *)malloc(buf_len);
        if (buf && httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            httpd_query_key_value(buf, "filename", raw_filename, sizeof(raw_filename));
        }
        if (buf) free(buf);
    }

    if (strlen(raw_filename) == 0) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    char decoded_filename[256];
    url_decode(decoded_filename, raw_filename, sizeof(decoded_filename));

    char filepath[320];
    snprintf(filepath, sizeof(filepath), "%s/%s", AIRDROP_BASE_PATH, decoded_filename);

    struct stat st;
    if (stat(filepath, &st) != 0) {
        snprintf(filepath, sizeof(filepath), "%s/%s", BSP_SD_MOUNT_POINT, decoded_filename);
        if (stat(filepath, &st) != 0) {
            ESP_LOGE(TAG, "File not found for view: %s", filepath);
            httpd_resp_send_404(req);
            return ESP_FAIL;
        }
    }

    FILE *fd = fopen(filepath, "rb");
    if (!fd) {
        ESP_LOGE(TAG, "Failed to open file for view: %s", filepath);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    int64_t total_size = (int64_t)st.st_size;
    const char *mime = get_mime_type(decoded_filename);

    char range_hdr[64] = {0};
    bool has_range = (httpd_req_get_hdr_value_str(req, "Range", range_hdr, sizeof(range_hdr)) == ESP_OK);

    int64_t start = 0;
    int64_t end = total_size - 1;
    bool is_partial = false;

    if (has_range) {
        long long r_start = 0, r_end = 0;
        if (sscanf(range_hdr, "bytes=%lld-%lld", &r_start, &r_end) == 2) {
            start = r_start;
            end = r_end;
            is_partial = true;
        } else if (sscanf(range_hdr, "bytes=%lld-", &r_start) == 1) {
            start = r_start;
            end = total_size - 1;
            is_partial = true;
        }
        if (start < 0) start = 0;
        if (end >= total_size) end = total_size - 1;
        if (start > end) start = end;
    }

    fseek(fd, (long)start, SEEK_SET);

    char hdr[384];
    int hdr_len = 0;

    if (is_partial) {
        hdr_len = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 206 Partial Content\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %lld\r\n"
            "Content-Range: bytes %lld-%lld/%lld\r\n"
            "Accept-Ranges: bytes\r\n"
            "Connection: keep-alive\r\n\r\n",
            mime, (long long)(end - start + 1), (long long)start, (long long)end, (long long)total_size);
    } else {
        hdr_len = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %lld\r\n"
            "Accept-Ranges: bytes\r\n"
            "Connection: keep-alive\r\n\r\n",
            mime, (long long)total_size);
    }

    if (httpd_send(req, hdr, hdr_len) <= 0) {
        fclose(fd);
        return ESP_FAIL;
    }

    int64_t remaining = end - start + 1;
    char *chunk = (char *)heap_caps_malloc(8192, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!chunk) chunk = (char *)malloc(4096);
    int chunk_size = chunk ? 8192 : 4096;

    if (!chunk) {
        fclose(fd);
        return ESP_FAIL;
    }

    while (remaining > 0) {
        size_t to_read = (size_t)MIN(remaining, (int64_t)chunk_size);
        size_t read_bytes = fread(chunk, 1, to_read, fd);
        if (read_bytes <= 0) break;
        int sent = httpd_send(req, chunk, read_bytes);
        if (sent <= 0) break;
        remaining -= sent;
    }

    free(chunk);
    fclose(fd);
    return ESP_OK;
}

// 5. Index page (GET /)
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

static const httpd_uri_t uri_view = {
    .uri       = "/view",
    .method    = HTTP_GET,
    .handler   = view_get_handler,
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
    config.max_open_sockets = 4;
    config.stack_size = 10240; // 10KB stack for safe FATFS/SDMMC file operations
    config.recv_wait_timeout = 15; // 15 seconds for larger file chunks over Wi-Fi
    config.send_wait_timeout = 15;

    ESP_LOGI(TAG, "Starting AirDrop HTTP Server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &uri_root);
        httpd_register_uri_handler(server, &uri_index);
        httpd_register_uri_handler(server, &uri_list);
        httpd_register_uri_handler(server, &uri_upload);
        httpd_register_uri_handler(server, &uri_download);
        httpd_register_uri_handler(server, &uri_view);
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
