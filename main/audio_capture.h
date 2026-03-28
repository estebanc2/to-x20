#pragma once
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"

#define AUDIO_SAMPLE_RATE   44100
#define AUDIO_FRAME_SIZE    256      /* muestras por canal por frame */
#define AUDIO_RINGBUF_SIZE  (AUDIO_FRAME_SIZE * 64 * sizeof(int16_t))

extern RingbufHandle_t g_audio_ringbuf;

esp_err_t audio_capture_init(void);
void      audio_capture_deinit(void);