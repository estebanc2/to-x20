#include "audio_capture.h"
#include "bt_audio.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "MAIN";

void app_main(void)
{
    ESP_LOGI(TAG, "=== ESP32 BT Audio Bridge ===");
    ESP_LOGI(TAG, "IDF version: %s", esp_get_idf_version());

    // 1. ADC — liviano, sin tareas
    ESP_ERROR_CHECK(audio_capture_init());

    // 2. Bluetooth A2DP Source
    ESP_ERROR_CHECK(bt_audio_start());
    
    /* Loop de monitoreo 
    while (1) {
        ESP_LOGI(TAG, "Heap libre: %" PRIu32 " bytes",
                 esp_get_free_heap_size());
        vTaskDelay(pdMS_TO_TICKS(5000));
    }*/
}
