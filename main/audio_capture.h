#pragma once
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"

#define AUDIO_SAMPLE_RATE   44100
#define AUDIO_FRAME_SIZE    256

// FIX: faltaba * 2 para canales estéreo — el buffer era la mitad del tamaño real.
// El original (FRAME_SIZE * 16 * sizeof(int16_t)) = 8192 bytes solo alcanzaba para
// 16 muestras mono. Ahora con * 2 * sizeof(int16_t) son frames estéreo completos.
// Se subió el multiplicador a 32 (antes 16) para dar más margen frente al jitter BT.
#define AUDIO_RINGBUF_SIZE  (AUDIO_FRAME_SIZE * 32 * 2 * sizeof(int16_t))
//                           256 frames        *32 *2ch *2 bytes = 32768 bytes

extern RingbufHandle_t g_audio_ringbuf;

esp_err_t audio_capture_init(void);
void audio_capture_deinit(void);
void audio_capture_set_rate(uint8_t sf_index);