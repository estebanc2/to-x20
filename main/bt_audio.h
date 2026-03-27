#pragma once
#include "esp_err.h"

#define BT_DEVICE_NAME   "ESP32 Audio Bridge"

/**
 * Inicializa Bluedroid y el perfil A2DP Source.
 * Queda en modo discovery esperando auriculares.
 */
esp_err_t bt_audio_start(void);

/** Detiene Bluetooth y libera recursos. */
void bt_audio_stop(void);