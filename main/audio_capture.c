#include "audio_capture.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "AUDIO_CAP";

static adc_oneshot_unit_handle_t s_adc = NULL;

#ifndef CLAMP
#define CLAMP(x, lo, hi) ((x)<(lo)?(lo):((x)>(hi)?(hi):(x)))
#endif

esp_err_t audio_capture_init(void)
{
    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = ADC_UNIT_1 };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &s_adc));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, ADC_CHANNEL_6, &chan_cfg));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, ADC_CHANNEL_7, &chan_cfg));

    ESP_LOGI(TAG, "ADC oneshot listo — esclavo del callback BT");
    return ESP_OK;
}

/**
 * Llamado desde a2dp_data_cb — lee exactamente (len/4) pares L+R.
 * len es siempre 512 bytes = 128 frames estéreo int16.
 */
void audio_capture_read(uint8_t *buf, int32_t len)
{
    int16_t *out    = (int16_t *)buf;
    int      frames = len / 4;  // 4 bytes por frame estéreo (2×int16)

    for (int i = 0; i < frames; i++) {
        int raw_l = 0, raw_r = 0;

        adc_oneshot_read(s_adc, ADC_CHANNEL_6, &raw_l);
        adc_oneshot_read(s_adc, ADC_CHANNEL_7, &raw_r);

        int32_t l = ((int32_t)raw_l - 2048) * 16;
        int32_t r = ((int32_t)raw_r - 2048) * 16;

        out[i * 2]     = (int16_t) CLAMP(l, -32768, 32767);
        out[i * 2 + 1] = (int16_t) CLAMP(r, -32768, 32767);
    }
}

void audio_capture_deinit(void)
{
    if (s_adc) {
        adc_oneshot_del_unit(s_adc);
        s_adc = NULL;
    }
}