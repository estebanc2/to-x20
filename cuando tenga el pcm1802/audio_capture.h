// audio_capture.h  —  ESP32 BT Audio Bridge (versión PCM1802 I2S)
#pragma once

#include "esp_err.h"
#include "freertos/ringbuf.h"

// Tamaño del ring buffer (igual que la versión ADC — bt_audio.c no cambia)
#define AUDIO_FRAME_SIZE     256
#define AUDIO_RINGBUF_SIZE   (AUDIO_FRAME_SIZE * 32 * sizeof(int16_t))

// Inicializa la captura I2S desde el PCM1802.
// Llamar DESPUÉS de bt_audio_init() para evitar colisiones con nvs_flash.
esp_err_t audio_capture_init(RingbufHandle_t ringbuf);

void audio_capture_deinit(void);