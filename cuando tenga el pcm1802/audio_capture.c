// audio_capture.c  —  ESP32 BT Audio Bridge
// Captura I2S desde PCM1802 (reemplaza la versión con ADC continuo)
//
// Conexiones:
//   PCM1802 BCK  → GPIO26
//   PCM1802 LRCK → GPIO25
//   PCM1802 DOUT → GPIO34  (I2S_DIN del ESP32)
//   PCM1802 SCKI → sin conectar (PCM1802 opera en modo slave)
//
// Straps del PCM1802:
//   FMT0 = GND, FMT1 = GND  → formato I2S estándar
//   SF0  = 3.3V, SF1  = GND → fs = 44.1 kHz (master clock por ESP32)
//   MD   = 3.3V             → modo esclavo (BCK y LRCK vienen del ESP32)
//
// El ESP32 actúa como I2S master: genera BCK y LRCK, el PCM1802 los sigue.
// MCLK (256×fs = ~11.289 MHz) se saca por GPIO0 usando el divisor de I2S.

#include "audio_capture.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "AUDIO_CAP";

// ── Parámetros ajustables ────────────────────────────────────────────────────
#define SAMPLE_RATE      44100          // Hz — coincide con SF0=VCC en PCM1802
#define GAIN             4              // Ganancia digital (PCM1802 ya tiene buen SNR,
                                        // valor bajo — subir a 8 si el volumen es bajo)
#define NOISE_GATE       16             // LSB — umbral bajo (ADC 24-bit, mucho menos ruido)
#define DMA_BUF_COUNT    8
#define DMA_BUF_FRAMES   256            // muestras por buffer DMA
// ────────────────────────────────────────────────────────────────────────────

// El PCM1802 entrega muestras de 24 bits empacadas en palabras de 32 bits (I2S)
// con los bits de datos en los 24 MSBs. Al leerlos como int32_t y hacer >>8
// obtenemos un int24 con signo en int32_t, que luego truncamos a int16_t.
#define I2S_BYTES_PER_FRAME   8         // 2 canales × 4 bytes/canal

static i2s_chan_handle_t  rx_chan = NULL;
static RingbufHandle_t    s_ringbuf = NULL;
static TaskHandle_t       s_task_handle = NULL;

// ── Filtro paso bajo IIR (igual que la versión ADC, coef conservador) ────────
static int32_t lp_l = 0, lp_r = 0;

static inline int32_t lpf(int32_t *state, int32_t sample)
{
    *state = (*state * 7 + sample) >> 3;
    return *state;
}

// ── Soft clipping (igual que antes) ─────────────────────────────────────────
static inline int16_t soft_clip(int32_t x)
{
    if (x >  32767) return  32767;
    if (x < -32768) return -32768;
    return (int16_t)x;
}

// ── Tarea de captura ─────────────────────────────────────────────────────────
static void capture_task(void *arg)
{
    // Buffer DMA: DMA_BUF_FRAMES estéreo × 4 bytes/muestra
    const size_t read_bytes = DMA_BUF_FRAMES * I2S_BYTES_PER_FRAME;
    int32_t *raw = (int32_t *)malloc(read_bytes);
    if (!raw) {
        ESP_LOGE(TAG, "Sin memoria para buffer DMA");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "I2S iniciado — PCM1802 @ %d Hz, I2S estándar 24-bit", SAMPLE_RATE);

    while (1) {
        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(rx_chan, raw, read_bytes,
                                          &bytes_read, portMAX_DELAY);
        if (err != ESP_OK || bytes_read == 0) continue;

        size_t frames = bytes_read / I2S_BYTES_PER_FRAME;

        // Salida mono: un int16_t por frame
        int16_t *out = (int16_t *)malloc(frames * sizeof(int16_t));
        if (!out) continue;

        for (size_t i = 0; i < frames; i++) {
            // raw[2i]   = canal L (24 bits en los MSBs del int32_t)
            // raw[2i+1] = canal R
            int32_t l24 = raw[2 * i]     >> 8;   // → int24 con signo
            int32_t r24 = raw[2 * i + 1] >> 8;

            // Mezcla mono (promedio L+R)
            int32_t mono = (l24 + r24) >> 1;

            // Filtro paso bajo IIR
            // Usamos un solo estado (mono), separamos L/R solo para el promedio
            mono = lpf(&lp_l, mono);

            // Noise gate
            if (mono > -NOISE_GATE && mono < NOISE_GATE) {
                lp_l = (lp_l * 7) >> 3;   // decay hacia 0
                mono = 0;
            }

            // Escalar de 24-bit a 16-bit con ganancia
            // mono está en rango ±2^23 → dividir por 2^8 para 16-bit, × GAIN
            int32_t s16 = (mono * GAIN) >> 8;

            out[i] = soft_clip(s16);
        }

        // Enviar al ring buffer (no bloqueante — descarta si está lleno)
        xRingbufferSend(s_ringbuf, out, frames * sizeof(int16_t), 0);
        free(out);
    }

    free(raw);
    vTaskDelete(NULL);
}

// ── Inicialización I2S ───────────────────────────────────────────────────────
esp_err_t audio_capture_init(RingbufHandle_t ringbuf)
{
    s_ringbuf = ringbuf;

    // Configuración del canal I2S RX
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(
        I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = DMA_BUF_COUNT;
    chan_cfg.dma_frame_num = DMA_BUF_FRAMES;

    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_chan));

    // Configuración estándar I2S
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_32BIT,   // 32-bit slot, 24-bit datos
                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = GPIO_NUM_0,    // MCLK para PCM1802 (256×fs ≈ 11.289 MHz)
            .bclk = GPIO_NUM_26,   // BCK
            .ws   = GPIO_NUM_25,   // LRCK
            .dout = I2S_GPIO_UNUSED,
            .din  = GPIO_NUM_34,   // DOUT del PCM1802
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_chan));

    // Tarea de captura en Core 1 (igual que la versión ADC)
    xTaskCreatePinnedToCore(capture_task, "audio_cap", 4096, NULL,
                            configMAX_PRIORITIES - 2, &s_task_handle, 1);

    return ESP_OK;
}

void audio_capture_deinit(void)
{
    if (s_task_handle) {
        vTaskDelete(s_task_handle);
        s_task_handle = NULL;
    }
    if (rx_chan) {
        i2s_channel_disable(rx_chan);
        i2s_del_channel(rx_chan);
        rx_chan = NULL;
    }
}