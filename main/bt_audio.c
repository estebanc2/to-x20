#include "bt_audio.h"
#include "audio_capture.h"

#include "nvs_flash.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <math.h>

static const char *TAG = "BT_AUDIO";

static bool s_connected = false;
static esp_bd_addr_t s_peer_addr;

/* ── Callback A2DP ──────────────────────────────────────────── */
static void a2dp_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    switch (event) {

    case ESP_A2D_CONNECTION_STATE_EVT: {
        if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            ESP_LOGI(TAG, "A2DP conectado: %02x:%02x:%02x:%02x:%02x:%02x",
                    param->conn_stat.remote_bda[0], param->conn_stat.remote_bda[1],
                    param->conn_stat.remote_bda[2], param->conn_stat.remote_bda[3],
                    param->conn_stat.remote_bda[4], param->conn_stat.remote_bda[5]);
            memcpy(s_peer_addr, param->conn_stat.remote_bda, sizeof(esp_bd_addr_t));
            s_connected = true;
            esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
        } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            ESP_LOGW(TAG, "A2DP desconectado — volviendo a discovery");
            s_connected = false;
            esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
        }
        break;
    }

    case ESP_A2D_AUDIO_STATE_EVT:
        ESP_LOGI(TAG, "Estado audio: %s",
                 param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED
                 ? "STARTED" : "STOPPED");
        break;

    case ESP_A2D_AUDIO_CFG_EVT: {
        uint8_t *sbc = (uint8_t *)&param->audio_cfg.mcc.cie;
        uint8_t sf = sbc[0] >> 6;
        const char *freq_str[] = {"16k", "32k", "44.1k", "48k"};
        ESP_LOGI(TAG, "SBC negociado — frecuencia: %s",
                sf < 4 ? freq_str[sf] : "?");
        extern void audio_capture_set_rate(uint8_t sf_index);
        audio_capture_set_rate(sf);
        break;
    }

    case ESP_A2D_MEDIA_CTRL_ACK_EVT:
        if (param->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY &&
            param->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
            ESP_LOGI(TAG, "Source listo — iniciando stream");
            esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
        } else if (param->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_START &&
                param->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
            ESP_LOGI(TAG, "Stream iniciado correctamente");
        }
        break;

    default:
        break;
    }
}

// FIX 3: Calibrar RESAMPLE_IN_RATE con el valor real de tu chip.
// 1. Flashear con el log de tasa real descomentado en audio_capture.c
// 2. Conectar al monitor serie (idf.py monitor)
// 3. Esperar 5 segundos y copiar el valor "Tasa ADC real: XXXXX Hz/canal"
// 4. Reemplazar 31909 abajo con ese valor y reflashear.
// Un error de ~500 Hz (~1.5%) hace que el audio suene ligeramente acelerado o frenado.
#define RESAMPLE_IN_RATE   31910   // ← reemplazar con valor medido del log
#define RESAMPLE_OUT_RATE  44100

static int32_t a2dp_data_cb(uint8_t *buf, int32_t len)
{
    if (!g_audio_ringbuf || !s_connected) {
        memset(buf, 0, len);
        return len;
    }

    // FIX 2: phase acumula correctamente entre callbacks.
    //
    // Bug original: al final del callback se hacía "phase &= 0xFFFF", lo que
    // conservaba solo la fracción sub-sample pero descartaba silenciosamente los
    // índices enteros consumidos. El próximo callback arrancaba con phase=fracción,
    // es decir desde el frame 0 del nuevo buffer, ignorando que ya había avanzado
    // parcialmente. Esto introducía micro-discontinuidades periódicas (glitches,
    // pequeños clicks) cada vez que el callback se completaba normalmente.
    //
    // Fix: se trackea el último índice entero consumido (last_idx) y al salir del
    // loop se sustrae esa parte entera de phase, conservando solo la fracción real.
    // phase representa ahora cuánto del primer frame del próximo buffer ya fue
    // "pre-consumido" en el callback anterior — es la forma correcta de mantener
    // la continuidad del resampleo entre llamadas.
    static uint32_t phase = 0;
    const uint32_t  phase_inc = (uint32_t)(((uint64_t)RESAMPLE_IN_RATE << 16) / RESAMPLE_OUT_RATE);

    int out_frames     = len / 4;
    int in_frames_need = (int)(((uint64_t)(out_frames + 4) * RESAMPLE_IN_RATE) / RESAMPLE_OUT_RATE) + 4;
    int in_bytes_need  = in_frames_need * 4;

    size_t   received = 0;
    uint8_t *raw = xRingbufferReceiveUpTo(g_audio_ringbuf, &received,
                                           pdMS_TO_TICKS(5),
                                           (size_t)in_bytes_need);

    if (!raw || received < 8) {
        if (raw) vRingbufferReturnItem(g_audio_ringbuf, raw);
        memset(buf, 0, len);
        phase = 0;
        return len;
    }

    int16_t *in  = (int16_t *)raw;
    int16_t *out = (int16_t *)buf;
    int in_frames_got = (int)(received / 4);

    int last_idx = 0;  // último índice entero consumido en este callback

    for (int i = 0; i < out_frames; i++) {
        uint32_t idx  = phase >> 16;
        uint32_t frac = phase & 0xFFFF;

        if ((int)idx + 1 >= in_frames_got) {
            // Fin del buffer de entrada: rellenar con la última muestra válida
            int last = in_frames_got - 1;
            if (last < 0) last = 0;
            for (int j = i; j < out_frames; j++) {
                out[j * 2]     = in[last * 2];
                out[j * 2 + 1] = in[last * 2 + 1];
            }
            last_idx = last;
            // Conservar solo fracción — el próximo buffer empieza desde 0
            phase = frac;
            goto done;
        }

        // Interpolación lineal entre frame idx y idx+1
        int32_t l0 = in[idx * 2],     l1 = in[(idx + 1) * 2];
        int32_t r0 = in[idx * 2 + 1], r1 = in[(idx + 1) * 2 + 1];

        out[i * 2]     = (int16_t)(l0 + ((l1 - l0) * (int32_t)frac >> 16));
        out[i * 2 + 1] = (int16_t)(r0 + ((r1 - r0) * (int32_t)frac >> 16));

        last_idx = (int)idx;
        phase += phase_inc;
    }

    // FIX 2: restar la parte entera consumida, conservar solo la fracción.
    // Sin este fix phase &= 0xFFFF descartaba los índices y causaba glitches.
    phase -= ((uint32_t)last_idx << 16);

done:
    vRingbufferReturnItem(g_audio_ringbuf, raw);
    return len;
}

/* ── Callback GAP (discovery) ───────────────────────────────── */
static void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    if (event == ESP_BT_GAP_DISC_RES_EVT) {
        ESP_LOGI(TAG, "Dispositivo encontrado — intentando A2DP...");
        esp_a2d_source_connect(param->disc_res.bda);
    }
}

/* ── API pública ────────────────────────────────────────────── */
esp_err_t bt_audio_start(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    esp_bt_gap_set_device_name(BT_DEVICE_NAME);

    ESP_ERROR_CHECK(esp_bt_gap_register_callback(gap_cb));
    ESP_ERROR_CHECK(esp_a2d_register_callback(a2dp_cb));
    ESP_ERROR_CHECK(esp_a2d_source_register_data_callback(a2dp_data_cb));
    ESP_ERROR_CHECK(esp_a2d_source_init());

    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

    ESP_LOGI(TAG, "Iniciando discovery — pon auriculares en modo pairing");
    esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);

    return ESP_OK;
}

void bt_audio_stop(void)
{
    if (s_connected) esp_a2d_source_disconnect(s_peer_addr);
    esp_a2d_source_deinit();
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
}