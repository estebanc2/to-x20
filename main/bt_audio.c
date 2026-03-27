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
#include "freertos/ringbuf.h"
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
        /* El sink nos informa de la config SBC negociada */
        ESP_LOGI(TAG, "Config SBC negociada");
        break;

    default:
        break;
    }
}

/* ── Callback de datos: ESP-IDF pide muestras PCM ──────────── */
static int32_t a2dp_data_cb(uint8_t *buf, int32_t len)
{
    if (!g_audio_ringbuf || !s_connected) {
        memset(buf, 0, len);
        return len;
    }

    size_t received = 0;
    uint8_t *data = xRingbufferReceiveUpTo(
        g_audio_ringbuf, &received,
        pdMS_TO_TICKS(5),
        (size_t)len
    );

    if (data && received > 0) {
        memcpy(buf, data, received);
        vRingbufferReturnItem(g_audio_ringbuf, data);

        /* Rellenar resto con silencio si el buffer no tenía suficiente */
        if ((int32_t)received < len) {
            memset(buf + received, 0, len - received);
        }
    } else {
        /* Sin datos: silencio */
        memset(buf, 0, len);
    }

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