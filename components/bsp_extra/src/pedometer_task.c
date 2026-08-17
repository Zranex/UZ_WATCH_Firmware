#include "pedometer_task.h"
#include "qmi8658.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <math.h>

static const char *TAG = "pedometer_task";

// Global step counter
static volatile uint32_t step_count = 0;

// Algorithm parameters
#define PEDOMETER_TASK_DELAY_MS 20      // 50 Hz sampling rate
#define PEAK_THRESHOLD 1.2f             // 1.2 G to trigger a step (Earth gravity is 1.0 G)
#define COOLDOWN_MS 300                 // Minimum time between steps

static void pedometer_task(void *pvParameter)
{
    qmi8658_acc_t acc;
    float filtered_mag = 1.0f;          // Initialize with 1G
    float alpha = 0.2f;                 // Low pass filter coefficient
    
    TickType_t last_step_time = 0;

    ESP_LOGI(TAG, "Pedometer task started");

    while (1) {
        if (qmi8658_read_acc(&acc) == ESP_OK) {
            // Calculate vector magnitude
            float mag = sqrtf((acc.x * acc.x) + (acc.y * acc.y) + (acc.z * acc.z));
            
            // Low pass filter to smooth out noise
            filtered_mag = (alpha * mag) + ((1.0f - alpha) * filtered_mag);
            
            // Peak detection
            if (filtered_mag > PEAK_THRESHOLD) {
                TickType_t current_time = xTaskGetTickCount();
                
                // Check cooldown to prevent double counting
                if ((current_time - last_step_time) * portTICK_PERIOD_MS > COOLDOWN_MS) {
                    step_count++;
                    last_step_time = current_time;
                    ESP_LOGD(TAG, "Step detected! Total: %lu", step_count);
                }
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(PEDOMETER_TASK_DELAY_MS));
    }
}

esp_err_t pedometer_task_init(void)
{
    // Make sure QMI8658 is initialized before we start reading it
    // Note: bsp_extra_init() already initializes it, so we are safe here
    
    BaseType_t ret = xTaskCreatePinnedToCore(
        pedometer_task,
        "pedometer_task",
        4096,
        NULL,
        5, // Priority
        NULL,
        1  // Run on Core 1 (App Core) to not block Wi-Fi/BLE
    );
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create pedometer task");
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

uint32_t pedometer_get_steps(void)
{
    return step_count;
}

void pedometer_reset_steps(void)
{
    step_count = 0;
}
