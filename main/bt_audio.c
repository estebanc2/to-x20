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

    case ESP_A2D_AUDIO_CFG_EVT: 
        uint8_t *sbc = (uint8_t *)&param->audio_cfg.mcc.cie;
        uint8_t sf = sbc[0] >> 6;
        const char *freq_str[] = {"16k", "32k", "44.1k", "48k"};
        ESP_LOGI(TAG, "SBC negociado — frecuencia: %s",
                sf < 4 ? freq_str[sf] : "?");
        // sf==0→16k, sf==1→32k, sf==2→44.1k, sf==3→48k
        // Guardamos para ajustar el ADC
        extern void audio_capture_set_rate(uint8_t sf_index);
        audio_capture_set_rate(sf);
        break;

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
// Ratio de resampleo: cuántas muestras ADC por cada muestra BT
// ADC produce ~31909 Hz, BT espera 44100 Hz
// ratio = 31909 / 44100 = 0.7235 — por cada muestra BT tomamos 0.7235 del ADC
#define RESAMPLE_IN_RATE   31909
#define RESAMPLE_OUT_RATE  44100

static int32_t a2dp_data_cb(uint8_t *buf, int32_t len)
{
    if (!g_audio_ringbuf || !s_connected) {
        memset(buf, 0, len);
        return len;
    }

    int out_frames = len / 4;  // frames estéreo int16 que pide BT

    // Cuántos frames del ADC necesitamos para producir out_frames a 44100 Hz
    int in_frames_needed = (int)((int64_t)out_frames * RESAMPLE_IN_RATE / RESAMPLE_OUT_RATE) + 2;
    int in_bytes_needed  = in_frames_needed * 4;

    // Leer del ring buffer
    size_t received = 0;
    uint8_t *raw = xRingbufferReceiveUpTo(g_audio_ringbuf, &received,
                                           pdMS_TO_TICKS(5),
                                           (size_t)in_bytes_needed);
    if (!raw || received < 4) {
        if (raw) vRingbufferReturnItem(g_audio_ringbuf, raw);
        memset(buf, 0, len);
        return len;
    }

    int16_t *in  = (int16_t *)raw;
    int16_t *out = (int16_t *)buf;
    int in_frames_got = (int)(received / 4);

    // Resampleo lineal
    // Acumulador de fase en punto fijo (16 bits fraccionarios)
    static uint32_t phase = 0;  // posición fraccionaria en el buffer de entrada
    const uint32_t phase_inc = (uint32_t)(((uint64_t)RESAMPLE_IN_RATE << 16) / RESAMPLE_OUT_RATE);

    for (int i = 0; i < out_frames; i++) {
        uint32_t idx  = phase >> 16;
        uint32_t frac = phase & 0xFFFF;

        if ((int)idx + 1 >= in_frames_got) {
            // Sin datos suficientes — silencio para el resto
            for (int j = i; j < out_frames; j++) {
                out[j * 2]     = 0;
                out[j * 2 + 1] = 0;
            }
            break;
        }

        // Interpolación lineal entre muestra idx e idx+1
        int32_t l0 = in[idx * 2],     l1 = in[(idx + 1) * 2];
        int32_t r0 = in[idx * 2 + 1], r1 = in[(idx + 1) * 2 + 1];

        out[i * 2]     = (int16_t)(l0 + (int32_t)((l1 - l0) * frac >> 16));
        out[i * 2 + 1] = (int16_t)(r0 + (int32_t)((r1 - r0) * frac >> 16));

        phase += phase_inc;
    }

    // Resetear phase para el próximo callback (relativa al buffer consumido)
    uint32_t consumed_idx = phase >> 16;
    phase -= (consumed_idx << 16);  // guardar solo la fracción restante

    vRingbufferReturnItem(g_audio_ringbuf, raw);
    return len;
}

/* ── Callback GAP (discovery) ───────────────────────────────── */
static void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    if (event == ESP_BT_GAP_DISC_RES_EVT) {
        /* Intentar conectar con cualquier dispositivo que encuentre */
        ESP_LOGI(TAG, "Dispositivo encontrado — intentando A2DP...");
        esp_a2d_source_connect(param->disc_res.bda);
    }
}

/* ── API pública ────────────────────────────────────────────── */
esp_err_t bt_audio_start(void)
{
    /* NVS requerido por BT */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* Liberar memoria BLE (no la usamos) */
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    /* Nombre visible */
    esp_bt_gap_set_device_name(BT_DEVICE_NAME);
    
    /* Registrar callbacks */
    ESP_ERROR_CHECK(esp_bt_gap_register_callback(gap_cb));
    ESP_ERROR_CHECK(esp_a2d_register_callback(a2dp_cb));
    ESP_ERROR_CHECK(esp_a2d_source_register_data_callback(a2dp_data_cb));
    ESP_ERROR_CHECK(esp_a2d_source_init());

    /* Hacer el dispositivo descubrible/conectable */
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

    /* Iniciar búsqueda de auriculares */
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