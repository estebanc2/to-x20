# ESP32 BT Audio Bridge

Convierte un ESP32 en un adaptador de audio Bluetooth: recibe audio estéreo analógico desde un jack de 3.5 mm y lo transmite por Bluetooth A2DP a auriculares inalámbricos.

```
[Jack 3.5 mm estéreo]
    Canal L → GPIO34
    Canal R → GPIO35        →  [ADC continuo DMA]  →  [Resampleo]  →  [A2DP / SBC]  →  [Auriculares BT]
    GND     → GND
```

---

## Requisitos

### Hardware

| Componente | Detalle |
|---|---|
| **ESP32 original** (no C3, no S3) | WROOM-32, WROVER, DevKit, etc. |
| Cable / conector jack 3.5 mm | TRS o TRRS estéreo |
| 2× resistencia 10 kΩ | Divisor de tensión por canal |
| 2× condensador electrolítico 10 µF | Bloqueo de DC por canal |
| Protoboard + cables | Para el circuito de acondicionamiento |
| Auriculares Bluetooth | Compatibles con A2DP (la mayoría lo son) |

> ⚠️ **Solo funciona en ESP32 original.** El ESP32-C3, C6, S2 y S3 no tienen Bluetooth clásico ni soporte A2DP Source.

### Software

| Herramienta | Versión mínima | Enlace |
|---|---|---|
| ESP-IDF | **v5.1** | https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/ |
| VSCode | cualquier reciente | https://code.visualstudio.com |
| Espressif IDF Extension | v1.7+ | Marketplace de VSCode |

> ⚠️ **No uses ESP-IDF v4.x.** Las APIs `esp_adc/adc_continuous.h` y el sistema de calibración ADC moderno requieren v5.0 o superior.

---

## Estructura del proyecto

```
esp32_bt_audio/
├── CMakeLists.txt          # CMake raíz
├── sdkconfig.defaults      # Configuración Kconfig predefinida
└── main/
    ├── CMakeLists.txt      # Componente principal
    ├── main.c              # Punto de entrada (app_main)
    ├── audio_capture.c     # ADC continuo DMA + ring buffer + filtros
    ├── audio_capture.h
    ├── bt_audio.c          # Stack Bluedroid + A2DP Source + resampleo
    └── bt_audio.h
```

---

## Circuito de acondicionamiento de señal

El ADC del ESP32 acepta entre **0 V y 3.3 V**. La señal de audio de un jack varía entre −1 V y +1 V centrada en 0 V, por lo que hay que desplazarla a ~1.65 V con este circuito (repetir para cada canal):

```
3.3V ──[R1 10kΩ]──┬─────────────── GPIO34 (Canal L)
                  │
Jack L ──[C1 10µF]┘
                  │
               [R2 10kΩ]
                  │
                 GND

GND del jack ──── GND del ESP32
```

Para el canal derecho, el mismo esquema en GPIO35.

| Componente | Valor | Función |
|---|---|---|
| C1, C2 | 10 µF electrolítico | Bloquea la componente DC del jack |
| R1, R3 | 10 kΩ | Rama alta del divisor (conectada a 3.3 V) |
| R2, R4 | 10 kΩ | Rama baja del divisor (conectada a GND) |

> El divisor centra la señal en **1.65 V** (mitad de 3.3 V). El código calcula y resta el offset DC real automáticamente mediante un filtro de largo plazo.

---

## Arquitectura de audio

El ESP32 original tiene una limitación importante: el **ADC continuo no puede samplear a 44,100 Hz** cuando Bluetooth está activo. La tasa real medida es ~31,900 Hz/canal con `sample_freq_hz = 78000`.

Para resolver la discrepancia entre la tasa del ADC y la que espera el stack BT (44,100 Hz), el código implementa un **resampleador con interpolación lineal** en el callback `a2dp_data_cb`:

```
ADC DMA → capture_task → ring buffer → a2dp_data_cb → resampleo 31900→44100 Hz → stack SBC
```

### Cadena de procesamiento de audio

Dentro de `capture_task` cada muestra pasa por:

1. **Cancelación de DC** — filtro IIR de muy baja frecuencia que estima y resta el offset DC del circuito (τ ≈ 256 muestras)
2. **Noise gate** — silencia señales menores a ±30 LSB (ruido de fondo del ADC)
3. **Filtro paso bajo IIR** — atenúa ruido de alta frecuencia del ADC (coef 15/16)
4. **Ganancia** — amplificación digital × 96 para compensar el rango limitado del divisor de tensión

### Orden real de los canales ADC

El hardware ESP32 entrega los canales en orden inverso al configurado: **CH7 (R) llega primero, CH6 (L) segundo**. El código ya lo tiene en cuenta.

---

## sdkconfig.defaults

```
CONFIG_BT_ENABLED=y
CONFIG_BT_CLASSIC_ENABLED=y
CONFIG_BT_A2DP_ENABLE=y
CONFIG_BT_SPP_ENABLED=n
CONFIG_BT_BLE_ENABLED=n
CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY=y
CONFIG_ADC_CAL_EFUSE_TP_ENABLE=y
CONFIG_ADC_CAL_EFUSE_VREF_ENABLE=y
CONFIG_ADC_CAL_LUT_ENABLE=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_LOG_DEFAULT_LEVEL_INFO=y
CONFIG_ADC_CONTINUOUS_ISR_IRAM_SAFE=y
CONFIG_SPI_FLASH_AUTO_SUSPEND=n
```

> `ADC_CONTINUOUS_ISR_IRAM_SAFE` y `SPI_FLASH_AUTO_SUSPEND=n` son obligatorios — sin ellos el ISR del ADC DMA provoca un `Cache error` al ejecutarse mientras el flash está siendo accedido por el stack BT.

---

## Instalación y configuración

### 1. Seleccionar el target

```bash
idf.py set-target esp32
```

### 2. Compilar

```bash
idf.py fullclean
idf.py build
```

El `fullclean` es necesario cuando se modifica `sdkconfig.defaults`.

### 3. Flashear y monitorear

```bash
idf.py flash monitor
```

O por separado:

```bash
idf.py flash -p /dev/ttyUSB0
idf.py monitor -p /dev/ttyUSB0
```

En macOS el puerto suele ser `/dev/cu.usbserial-*`. En Windows `COM3`, `COM4`, etc.

---

## Uso

1. Conecta el jack 3.5 mm a la salida de auriculares de tu fuente de audio.
2. Alimenta el ESP32 (USB o 3.3 V externo).
3. **Pon tus auriculares Bluetooth en modo pairing.**
4. El ESP32 inicia un escaneo y se conecta automáticamente al primer dispositivo A2DP encontrado.
5. El discovery se relanza automáticamente si los auriculares se desconectan.
6. La salida serie (115200 baud) muestra el estado:

```
I (512) MAIN: === ESP32 BT Audio Bridge ===
I (512) AUDIO_CAP: ADC continuo iniciado
I (1162) BT_AUDIO: Iniciando discovery — pon auriculares en modo pairing
I (16722) BT_AUDIO: A2DP conectado: aa:bb:cc:dd:ee:ff
I (16842) BT_AUDIO: Stream iniciado correctamente
I (16852) BT_AUDIO: Estado audio: STARTED
```

---

## Parámetros ajustables

En `audio_capture.c`:

| Parámetro | Valor | Descripción |
|---|---|---|
| `GAIN` | `96` | Ganancia digital — subir si el volumen es bajo, bajar si distorsiona |
| `NOISE_GATE` | `30` | LSB — silencia señales por debajo de este umbral |
| `sample_freq_hz` | `78000` | Frecuencia de config del ADC — produce ~31,900 Hz/canal reales |

En `bt_audio.c`:

| Parámetro | Valor | Descripción |
|---|---|---|
| `RESAMPLE_IN_RATE` | `31909` | Tasa real medida del ADC (Hz/canal) |
| `RESAMPLE_OUT_RATE` | `44100` | Tasa que espera el stack BT |
| `BT_DEVICE_NAME` | `"ESP32 Audio Bridge"` | Nombre visible por Bluetooth |

En `audio_capture.h`:

| Parámetro | Valor | Descripción |
|---|---|---|
| `AUDIO_FRAME_SIZE` | `256` | Muestras por frame de captura |
| `AUDIO_RINGBUF_SIZE` | `FRAME_SIZE × 16 × 2` | Tamaño del ring buffer |

---

## Asignación de núcleos (dual-core)

| Tarea | Núcleo | Prioridad | Motivo |
|---|---|---|---|
| Stack Bluedroid / A2DP | Core 0 | alta (gestionada por IDF) | Reservado para BT |
| `audio_cap` (ADC DMA → ring buffer) | Core 1 | `MAX_PRIORITIES − 2` | No interfiere con BT |
| `app_main` / monitoreo | Core 1 | normal | Tarea secundaria |

> BT debe inicializarse **antes** que el ADC continuo para que `nvs_flash_init` no colisione con el ISR del DMA.

---

## Limitaciones conocidas

- **Tasa de muestreo ADC limitada:** Con BT activo el ADC continuo produce ~31,900 Hz/canal en lugar de los 44,100 Hz declarados al stack BT. Se compensa con resampleo por interpolación lineal, pero introduce una leve degradación de calidad.
- **Calidad de audio del ADC interno:** El ADC del ESP32 tiene ruido inherente. Para mayor fidelidad considera un códec externo I2S como el PCM1802 o el ES8388.
- **Un solo dispositivo BT simultáneo:** A2DP Source solo admite una conexión activa.
- **Sin emparejamiento por nombre:** Se conecta al primer dispositivo A2DP encontrado. Para conectarse siempre a los mismos auriculares, guarda su MAC y filtra en el callback GAP.
- **Latencia:** Entre 100 ms y 300 ms dependiendo del firmware de los auriculares.

---

## Solución de problemas

| Síntoma | Causa probable | Solución |
|---|---|---|
| No encuentra auriculares | Auriculares no en modo pairing | Ponlos en pairing antes de que termine el discovery (~10 s) |
| `Cache error` al arrancar | `sdkconfig.defaults` incompleto | Verificar `ADC_CONTINUOUS_ISR_IRAM_SAFE=y` y `SPI_FLASH_AUTO_SUSPEND=n`, luego `idf.py fullclean` |
| Audio acelerado / efecto ardilla | `RESAMPLE_IN_RATE` incorrecto | Medir la tasa real con el log `Tasa ADC real` y actualizar la constante |
| Audio muy bajo | `GAIN` insuficiente | Subir `GAIN` en `audio_capture.c` (valor recomendado: 96) |
| Audio distorsionado | `GAIN` excesivo | Reducir `GAIN` |
| Audio opaco / sin agudos | Filtro IIR demasiado agresivo | Cambiar coef de `15/16` a `7/8` en `audio_capture.c` |
| Watchdog IDLE en `audio_cap` | Tarea ADC sin ceder CPU | Verificar que se usa ADC continuo con ISR, no oneshot en loop |
| Error de compilación en ESP32-C3/S3 | Chip sin BT clásico | Usar ESP32 original únicamente |
| `CONFIG_BT_A2DP_ENABLE` no disponible | BT clásico desactivado | Ejecutar `idf.py menuconfig` y activar BT clásico |

---

## Licencia

MIT — libre para uso personal y comercial.
