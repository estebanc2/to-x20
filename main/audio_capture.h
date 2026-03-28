#pragma once
#include "esp_err.h"
#include <stdint.h>

esp_err_t audio_capture_init(void);
void      audio_capture_read(uint8_t *buf, int32_t len);
void      audio_capture_deinit(void);