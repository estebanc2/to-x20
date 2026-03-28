#include "audio_capture.h"

#include "esp_adc/adc_continuous.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include <string.h>

static const char *TAG = "AUDIO_CAP";

RingbufHandle_t g_audio_ringbuf = NULL;

static adc_continuous_handle_t s_adc_handle = NULL;
static TaskHandle_t             s_task       = NULL;
static bool                     s_running    = false;

#ifndef CLAMP
#define CLAMP(x, lo, hi) ((x)<(lo)?(lo):((x)>(hi)?(hi):(x)))
#endif

static bool IRAM_ATTR adc_conv_done_cb(adc_continuous_handle_t handle,
                                        const adc_continuous_evt_data_t *edata,
                                        void *user_data)
{
    BaseType_t high_prio_woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_task, &high_prio_woken);
    return high_prio_woken == pdTRUE;
}

static void capture_task(void *arg)
{
    const size_t raw_size = SOC_ADC_DIGI_RESULT_BYTES * 2 * AUDIO_FRAME_SIZE;
    uint8_t *raw_buf = heap_caps_malloc(raw_size, MALLOC_CAP_DMA);
    assert(raw_buf);

    int16_t stereo[AUDIO_FRAME_SIZE * 2];

    ESP_ERROR_CHECK(adc_continuous_start(s_adc_handle));
    ESP_LOGI(TAG, "Tarea captura iniciada — %d Hz", AUDIO_SAMPLE_RATE);

    uint32_t loop_count = 0;
    uint32_t notify_count = 0;
    uint32_t frames_sent = 0;

    while (s_running) {
        loop_count++;
        BaseType_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
        if (notified) notify_count++;

        // LOG TEMPORAL cada 100 iteraciones
        if (loop_count % 100 == 0) {
            ESP_LOGI(TAG, "loops=%lu notificaciones=%lu frames_enviados=%lu",
                     (unsigned long)loop_count,
                     (unsigned long)notify_count,
                     (unsigned long)frames_sent);
        }

        uint32_t bytes_read = 0;
        if (adc_continuous_read(s_adc_handle, raw_buf, raw_size,
                                &bytes_read, 0) != ESP_OK || bytes_read == 0) continue;

        uint32_t n      = bytes_read / SOC_ADC_DIGI_RESULT_BYTES;
        uint32_t frames = 0;

        for (uint32_t i = 0; i + 1 < n && frames < (uint32_t)AUDIO_FRAME_SIZE; i += 2) {
            adc_digi_output_data_t *d0 =
                (adc_digi_output_data_t *)&raw_buf[i       * SOC_ADC_DIGI_RESULT_BYTES];
            adc_digi_output_data_t *d1 =
                (adc_digi_output_data_t *)&raw_buf[(i + 1) * SOC_ADC_DIGI_RESULT_BYTES];

            if (d0->type1.channel != ADC_CHANNEL_7 ||
                d1->type1.channel != ADC_CHANNEL_6) continue;

            int32_t l = ((int32_t)d1->type1.data - 2048) * 16; // L viene en d1
            int32_t r = ((int32_t)d0->type1.data - 2048) * 16; // R viene en d0

            stereo[frames * 2]     = (int16_t) CLAMP(l, -32768, 32767);
            stereo[frames * 2 + 1] = (int16_t) CLAMP(r, -32768, 32767);
            frames++;
        }

        if (frames > 0) {
            size_t free_space = xRingbufferGetCurFreeSize(g_audio_ringbuf);
            size_t needed     = frames * 2 * sizeof(int16_t);
            if (free_space < needed) {
                size_t drain_size;
                void *old = xRingbufferReceiveUpTo(g_audio_ringbuf, &drain_size, 0, needed - free_space);
                if (old) vRingbufferReturnItem(g_audio_ringbuf, old);
            }
            xRingbufferSend(g_audio_ringbuf, stereo, needed, 0);
            frames_sent += frames;
        }
    }

    adc_continuous_stop(s_adc_handle);
    free(raw_buf);
    vTaskDelete(NULL);
}

esp_err_t audio_capture_init(void)
{
    g_audio_ringbuf = xRingbufferCreate(AUDIO_RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF);
    if (!g_audio_ringbuf) return ESP_ERR_NO_MEM;

    adc_continuous_handle_cfg_t handle_cfg = {
        .max_store_buf_size = AUDIO_FRAME_SIZE * 8 * SOC_ADC_DIGI_RESULT_BYTES,
        .conv_frame_size    = AUDIO_FRAME_SIZE * 2 * SOC_ADC_DIGI_RESULT_BYTES,
    };
    ESP_ERROR_CHECK(adc_continuous_new_handle(&handle_cfg, &s_adc_handle));

    adc_digi_pattern_config_t pattern[2] = {
        { .atten = ADC_ATTEN_DB_12, .channel = ADC_CHANNEL_6,
          .unit = ADC_UNIT_1, .bit_width = ADC_BITWIDTH_12 },
        { .atten = ADC_ATTEN_DB_12, .channel = ADC_CHANNEL_7,
          .unit = ADC_UNIT_1, .bit_width = ADC_BITWIDTH_12 },
    };

    adc_continuous_config_t cont_cfg = {
        // ESP32 soporta múltiplos exactos del clock interno
        // 44100*2 no es exacto — usar 80000 (40000 por canal) como aproximación
        // o mejor: dejar que el BT marque el ritmo y no usar frecuencia fija
        .sample_freq_hz = 80000,   // 40000 por canal ≈ suficiente para 44100
        .conv_mode      = ADC_CONV_SINGLE_UNIT_1,
        .format         = ADC_DIGI_OUTPUT_FORMAT_TYPE1,
        .pattern_num    = 2,
        .adc_pattern    = pattern,
    };
    ESP_ERROR_CHECK(adc_continuous_config(s_adc_handle, &cont_cfg));

    adc_continuous_evt_cbs_t cbs = { .on_conv_done = adc_conv_done_cb };
    ESP_ERROR_CHECK(adc_continuous_register_event_callbacks(s_adc_handle, &cbs, NULL));

    s_running = true;
    xTaskCreatePinnedToCore(capture_task, "audio_cap",
                            4096, NULL,
                            configMAX_PRIORITIES - 2,
                            &s_task, 1);
    return ESP_OK;
}

void audio_capture_deinit(void)
{
    s_running = false;
    vTaskDelay(pdMS_TO_TICKS(200));
    adc_continuous_deinit(s_adc_handle);
    if (g_audio_ringbuf) {
        vRingbufferDelete(g_audio_ringbuf);
        g_audio_ringbuf = NULL;
    }
}