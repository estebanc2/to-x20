#include "audio_capture.h"
#include "esp_timer.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include <string.h>

static const char *TAG = "AUDIO_CAP";

RingbufHandle_t g_audio_ringbuf = NULL;

static adc_oneshot_unit_handle_t s_adc_handle;
static adc_cali_handle_t         s_cali_l, s_cali_r;
static TaskHandle_t               s_capture_task = NULL;
static bool                       s_running = false;

/* ── Calibración ────────────────────────────────────────────── */
static bool adc_calibration_init(adc_unit_t unit,
                                  adc_channel_t channel,
                                  adc_atten_t atten,
                                  adc_cali_handle_t *out)
{
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cfg = {
        .unit_id  = unit,
        .chan     = channel,
        .atten    = atten,
        .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_cali_create_scheme_curve_fitting(&cfg, out) == ESP_OK) {
        ESP_LOGI(TAG, "Calibración curve-fitting OK (ch%d)", channel);
        return true;
    }
#endif
#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cfg = {
        .unit_id  = unit,
        .atten    = atten,
        .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_cali_create_scheme_line_fitting(&cfg, out) == ESP_OK) {
        ESP_LOGI(TAG, "Calibración line-fitting OK (ch%d)", channel);
        return true;
    }
#endif
    ESP_LOGW(TAG, "Sin calibración para canal %d", channel);
    return false;
}

#ifndef CLAMP
#define CLAMP(x, lo, hi)  ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))
#endif

/* ── Tarea de captura ───────────────────────────────────────── */
static void capture_task(void *arg)
{
    int16_t frame[AUDIO_FRAME_SIZE * 2];

    // Periodo en ticks — cuánto tarda un frame completo
    const TickType_t frame_ticks = pdMS_TO_TICKS(
        (AUDIO_FRAME_SIZE * 1000) / AUDIO_SAMPLE_RATE
    );  // 512 muestras a 44100 Hz ≈ 11.6 ms → 12 ticks

    ESP_LOGI(TAG, "Tarea captura iniciada — %d Hz, frame_ticks=%lu",
             AUDIO_SAMPLE_RATE, (unsigned long)frame_ticks);

    TickType_t last_wake = xTaskGetTickCount();

    while (s_running) {

        for (int i = 0; i < AUDIO_FRAME_SIZE; i++) {
            int raw_l = 0, raw_r = 0;
            int tmp;

            for (int k = 0; k < AUDIO_OVERSAMPLE; k++) {
                adc_oneshot_read(s_adc_handle, AUDIO_ADC_LEFT_CH,  &tmp);
                raw_l += tmp;
                adc_oneshot_read(s_adc_handle, AUDIO_ADC_RIGHT_CH, &tmp);
                raw_r += tmp;
            }
            raw_l /= AUDIO_OVERSAMPLE;
            raw_r /= AUDIO_OVERSAMPLE;

            int32_t s_l = ((int32_t)raw_l - 2048) * 16;
            int32_t s_r = ((int32_t)raw_r - 2048) * 16;

            frame[i * 2]     = (int16_t) CLAMP(s_l, -32768, 32767);
            frame[i * 2 + 1] = (int16_t) CLAMP(s_r, -32768, 32767);
        }

        xRingbufferSend(g_audio_ringbuf,
                        frame,
                        sizeof(frame),
                        pdMS_TO_TICKS(5));

        // Cede al scheduler y mantiene el periodo correcto
        vTaskDelayUntil(&last_wake, frame_ticks);
    }

    ESP_LOGI(TAG, "Tarea captura finalizada");
    vTaskDelete(NULL);
}

/* ── API pública ────────────────────────────────────────────── */
esp_err_t audio_capture_init(void)
{
    /* Ring buffer */
    g_audio_ringbuf = xRingbufferCreate(AUDIO_RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF);
    if (!g_audio_ringbuf) {
        ESP_LOGE(TAG, "Error creando ring buffer");
        return ESP_ERR_NO_MEM;
    }

    /* ADC oneshot unit */
    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = ADC_UNIT_1 };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &s_adc_handle));

    /* Configurar canales */
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = ADC_ATTEN_DB_12,   /* 0–3.3 V */
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_handle, AUDIO_ADC_LEFT_CH,  &chan_cfg));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_handle, AUDIO_ADC_RIGHT_CH, &chan_cfg));

    /* Calibración */
    adc_calibration_init(ADC_UNIT_1, AUDIO_ADC_LEFT_CH,  ADC_ATTEN_DB_12, &s_cali_l);
    adc_calibration_init(ADC_UNIT_1, AUDIO_ADC_RIGHT_CH, ADC_ATTEN_DB_12, &s_cali_r);

    /* Arrancar tarea en core 1 (core 0 = BT stack) */
    s_running = true;
    BaseType_t ret = xTaskCreatePinnedToCore(
        capture_task, "audio_cap",
        4096, NULL,
        configMAX_PRIORITIES - 2,
        &s_capture_task, 1
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Error creando tarea de captura");
        return ESP_FAIL;
    }

    return ESP_OK;
}

void audio_capture_deinit(void)
{
    s_running = false;
    vTaskDelay(pdMS_TO_TICKS(200));
    adc_oneshot_del_unit(s_adc_handle);
    if (g_audio_ringbuf) {
        vRingbufferDelete(g_audio_ringbuf);
        g_audio_ringbuf = NULL;
    }
}