#include "audio_capture.h"
#include "esp_adc/adc_continuous.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include <string.h>
#include "esp_timer.h"

static const char *TAG = "AUDIO_CAP";

RingbufHandle_t g_audio_ringbuf = NULL;

static adc_continuous_handle_t s_adc = NULL;
static TaskHandle_t             s_task = NULL;
static bool                     s_running = false;

#ifndef CLAMP
#define CLAMP(x, lo, hi) ((x)<(lo)?(lo):((x)>(hi)?(hi):(x)))
#endif

// Parámetros ajustables
// GAIN=32: el divisor resistivo 10k/10k limita el rango de entrada; con 64 clippeaba.
//          Si el volumen sigue bajo, subir a 48. Si distorsiona, bajar a 24.
// NOISE_GATE=40: cubre el ruido de fondo del ADC interno (~15-25 LSB en reposo).
//          Si corta audio legítimo (voces suaves), bajar a 25.

#define GAIN        32
#define NOISE_GATE  40

// Soft clipping: comprime la cresta gradualmente en vez de cortarla duro.
// El hard CLAMP anterior producía distorsión tipo crunch en señales fuertes.
// Empieza a aplanar a partir de ±28000 con pendiente 1/4 — suena mucho más natural.
static inline int16_t soft_clip(int32_t x)
{
    if (x >  28000) x =  28000 + ((x -  28000) >> 2);
    if (x < -28000) x = -28000 + ((x + 28000) >> 2);
    return (int16_t) CLAMP(x, -32767, 32767);
}

static bool IRAM_ATTR adc_conv_done_cb(adc_continuous_handle_t handle,
                                        const adc_continuous_evt_data_t *edata,
                                        void *user_data)
{
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_task, &woken);
    return woken == pdTRUE;
}

static void capture_task(void *arg)
{
    const size_t raw_size = SOC_ADC_DIGI_RESULT_BYTES * 2 * AUDIO_FRAME_SIZE;
    uint8_t *raw_buf = heap_caps_malloc(raw_size, MALLOC_CAP_DMA);
    assert(raw_buf);

    int16_t stereo[AUDIO_FRAME_SIZE * 2];

    ESP_ERROR_CHECK(adc_continuous_start(s_adc));
    ESP_LOGI(TAG, "ADC continuo iniciado");

    // Medición de tasa real — necesaria para calibrar RESAMPLE_IN_RATE en bt_audio.c.
    // Conectar al monitor serie, esperar 5 s, copiar el valor y pegarlo en bt_audio.c.
    uint32_t total_frames = 0;
    int64_t  t_start      = esp_timer_get_time();

    while (s_running) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));

        uint32_t bytes_read = 0;
        if (adc_continuous_read(s_adc, raw_buf, raw_size, &bytes_read, 0) != ESP_OK
            || bytes_read == 0) continue;

        uint32_t n = bytes_read / SOC_ADC_DIGI_RESULT_BYTES;
        uint32_t frames = 0;

        static int32_t lp_l = 0, lp_r = 0;

        for (uint32_t i = 0; i + 1 < n && frames < (uint32_t)AUDIO_FRAME_SIZE; i += 2) {
            adc_digi_output_data_t *d0 =
                (adc_digi_output_data_t *)&raw_buf[i       * SOC_ADC_DIGI_RESULT_BYTES];
            adc_digi_output_data_t *d1 =
                (adc_digi_output_data_t *)&raw_buf[(i + 1) * SOC_ADC_DIGI_RESULT_BYTES];

            int32_t raw_l = (int32_t)d1->type1.data - 2048;
            int32_t raw_r = (int32_t)d0->type1.data - 2048;

            // Noise gate sobre la muestra cruda
            if (raw_l > -NOISE_GATE && raw_l < NOISE_GATE) raw_l = 0;
            if (raw_r > -NOISE_GATE && raw_r < NOISE_GATE) raw_r = 0;

            // Filtro paso bajo IIR coef 7/8 (fc ≈ 14 kHz)
            lp_l = (lp_l * 7 + raw_l) >> 3;
            lp_r = (lp_r * 7 + raw_r) >> 3;

            // FIX ruido de moto: cuando la muestra cruda es silencio, drenar el
            // estado del filtro IIR hacia 0 con decay rápido (3/4 por muestra).
            // Sin esto el ruido del ADC (~15-25 LSB en reposo) se acumula en
            // lp_l/lp_r — el filtro tiene memoria — y sale amplificado por GAIN,
            // produciendo el zumbido constante incluso sin señal de entrada.
            if (raw_l == 0) lp_l = (lp_l * 3) >> 2;
            if (raw_r == 0) lp_r = (lp_r * 3) >> 2;

            int32_t l = lp_l * GAIN;
            int32_t r = lp_r * GAIN;

            // FIX clipping: soft_clip en vez de hard CLAMP
            stereo[frames * 2]     = soft_clip(l);
            stereo[frames * 2 + 1] = soft_clip(r);
            frames++;
        }

        total_frames += frames;

        // Log de tasa real cada 5 s — copiar valor en RESAMPLE_IN_RATE (bt_audio.c)
        int64_t elapsed = esp_timer_get_time() - t_start;
        if (elapsed >= 5000000LL) {
            uint32_t hz = (uint32_t)((int64_t)total_frames * 1000000LL / elapsed);
            ESP_LOGI(TAG, "Tasa ADC real: %lu Hz/canal (objetivo: 44100)", (unsigned long)hz);
            total_frames = 0;
            t_start = esp_timer_get_time();
        }

        if (frames > 0) {
            // Margen de 16 frames (~200 ms) para absorber jitter del stack BT
            size_t max_bytes = 16 * AUDIO_FRAME_SIZE * 2 * sizeof(int16_t);
            size_t used = AUDIO_RINGBUF_SIZE - xRingbufferGetCurFreeSize(g_audio_ringbuf);
            if (used > max_bytes) {
                size_t to_drain = used - max_bytes;
                size_t drained;
                void *old = xRingbufferReceiveUpTo(g_audio_ringbuf, &drained, 0, to_drain);
                if (old) vRingbufferReturnItem(g_audio_ringbuf, old);
            }
            xRingbufferSend(g_audio_ringbuf, stereo, frames * 2 * sizeof(int16_t), 0);
        }
    }

    adc_continuous_stop(s_adc);
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
    ESP_ERROR_CHECK(adc_continuous_new_handle(&handle_cfg, &s_adc));

    adc_digi_pattern_config_t pattern[2] = {
        { .atten = ADC_ATTEN_DB_12, .channel = ADC_CHANNEL_6,
          .unit = ADC_UNIT_1, .bit_width = ADC_BITWIDTH_12 },
        { .atten = ADC_ATTEN_DB_12, .channel = ADC_CHANNEL_7,
          .unit = ADC_UNIT_1, .bit_width = ADC_BITWIDTH_12 },
    };

    adc_continuous_config_t cont_cfg = {
        .sample_freq_hz = 78000,
        .conv_mode      = ADC_CONV_SINGLE_UNIT_1,
        .format         = ADC_DIGI_OUTPUT_FORMAT_TYPE1,
        .pattern_num    = 2,
        .adc_pattern    = pattern,
    };
    ESP_ERROR_CHECK(adc_continuous_config(s_adc, &cont_cfg));

    adc_continuous_evt_cbs_t cbs = { .on_conv_done = adc_conv_done_cb };
    ESP_ERROR_CHECK(adc_continuous_register_event_callbacks(s_adc, &cbs, NULL));

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
    adc_continuous_deinit(s_adc);
    if (g_audio_ringbuf) {
        vRingbufferDelete(g_audio_ringbuf);
        g_audio_ringbuf = NULL;
    }
}

void audio_capture_set_rate(uint8_t sf_index)
{
    const uint32_t target_hz[] = {16000, 32000, 44100, 48000};
    if (sf_index > 3) return;

    uint32_t target = target_hz[sf_index];
    uint32_t adc_freq = (uint32_t)(target * 2 * 10000 / 4090);

    ESP_LOGI(TAG, "Ajustando ADC para %lu Hz/canal — config: %lu",
             (unsigned long)target, (unsigned long)adc_freq);

    adc_continuous_stop(s_adc);

    adc_digi_pattern_config_t pattern[2] = {
        { .atten = ADC_ATTEN_DB_12, .channel = ADC_CHANNEL_6,
          .unit = ADC_UNIT_1, .bit_width = ADC_BITWIDTH_12 },
        { .atten = ADC_ATTEN_DB_12, .channel = ADC_CHANNEL_7,
          .unit = ADC_UNIT_1, .bit_width = ADC_BITWIDTH_12 },
    };
    adc_continuous_config_t cont_cfg = {
        .sample_freq_hz = adc_freq,
        .conv_mode      = ADC_CONV_SINGLE_UNIT_1,
        .format         = ADC_DIGI_OUTPUT_FORMAT_TYPE1,
        .pattern_num    = 2,
        .adc_pattern    = pattern,
    };
    adc_continuous_config(s_adc, &cont_cfg);
    adc_continuous_start(s_adc);
}