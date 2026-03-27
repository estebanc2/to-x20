#pragma once
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"

/* Pines ADC */
#define AUDIO_ADC_LEFT_CH    ADC_CHANNEL_6   /* GPIO34 */
#define AUDIO_ADC_RIGHT_CH   ADC_CHANNEL_7   /* GPIO35 */

/* Parámetros de muestreo */
#define AUDIO_SAMPLE_RATE    44100
#define AUDIO_FRAME_SIZE     512             /* muestras por frame */
#define AUDIO_OVERSAMPLE     4               /* promedio de N lecturas */
#define AUDIO_RINGBUF_SIZE   (AUDIO_FRAME_SIZE * 8 * sizeof(int16_t))

/* Handle del ring buffer compartido con bt_audio */
extern RingbufHandle_t g_audio_ringbuf;

/**
 * Inicializa ADC y arranca la tarea de captura.
 * Debe llamarse antes de bt_audio_start().
 */
esp_err_t audio_capture_init(void);

/** Detiene y libera recursos de captura. */
void audio_capture_deinit(void);