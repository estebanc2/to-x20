# ESP32 BT Audio Bridge

Convierte un ESP32 en un adaptador de audio Bluetooth: recibe audio estéreo analógico desde un jack de 3.5 mm y lo transmite por Bluetooth A2DP a auriculares inalámbricos en modo **mono** (L+R mezclados).

```
[Jack 3.5 mm estéreo]
    Canal L → GPIO34 (CH6)
    Canal R → GPIO35 (CH7)  →  [ADC continuo DMA]  →  [Mezcla mono + oversampling x2]  →  [Resampleo]  →  [A2DP / SBC]  →  [Auriculares BT]
    GND     → GND
```

---

## Requisitos

### Hardware

| Componente | Detalle |
| --- | --- |
| **ESP32 original** (no C3, no S3) | WROOM-32, WROVER, DevKit, etc. |
| Cable / conector jack 3.5 mm | TRS o TRRS estéreo |
| 2× resistencia 10 kΩ | Divisor de tensión por canal |
| 2× condensador electrolítico 10 µF | Bloqueo de DC por canal |
| Protoboard + cables | Para el circuito de acondicionamiento |
| Auriculares Bluetooth | Compatibles con A2DP (la mayoría lo son) |

> ⚠️ **Solo funciona en ESP32 original.** El ESP32-C3, C6, S2 y S3 no tienen Bluetooth clásico ni soporte A2DP Source.

### Software

| Herramienta | Versión mínima | Enlace |
| --- | --- | --- |
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
| --- | --- | --- |
| C1, C2 | 10 µF electrolítico | Bloquea la componente DC del jack |
| R1, R3 | 10 kΩ | Rama alta del divisor (conectada a 3.3 V) |
| R2, R4 | 10 kΩ | Rama baja del divisor (conectada a GND) |

> El divisor centra la señal en **1.65 V** (mitad de 3.3 V). El código calcula y resta el offset DC real automáticamente mediante un filtro de largo plazo.

---

## Arquitectura de audio

### Modo mono con oversampling x2

El output es **mono**: los canales L y R del jack se mezclan en una sola señal antes del procesamiento. Esto permite usar los dos canales ADC (CH6 y CH7) como **dos muestras simultáneas del mismo audio**, promediándolas para reducir el ruido de cuantización del ADC interno (~3 dB de mejora en SNR). El circuito hardware no cambia — ambos pines GPIO34 y GPIO35 siguen conectados.

### Limitación de tasa del ADC

El ESP32 original tiene una limitación importante: el **ADC continuo no puede samplear a 44,100 Hz** cuando Bluetooth está activo. La tasa real medida es ~31,900 Hz/canal con `sample_freq_hz = 78000`.

Para resolver la discrepancia entre la tasa del ADC y la que espera el stack BT (44,100 Hz), el código implementa un **resampleador con interpolación lineal** en el callback `a2dp_data_cb`:

```
ADC DMA → capture_task → ring buffer → a2dp_data_cb → resampleo ~31900→44100 Hz → stack SBC
```

### Cadena de procesamiento de audio

Dentro de `capture_task` cada par de muestras pasa por:

1. **Oversampling x2** — promedio de CH6 (L) y CH7 (R) → señal mono con menor ruido
2. **Noise gate** — silencia señales menores a ±40 LSB (ruido de fondo del ADC)
3. **Filtro paso bajo IIR** — atenúa ruido de alta frecuencia (coef 7/8, fc ≈ 14 kHz)
4. **Decay en silencio** — drena el estado del filtro hacia 0 cuando no hay señal, evitando que el ruido del ADC se amplifique como zumbido de fondo
5. **Ganancia** — amplificación digital × 32
6. **Soft clipping** — compresión suave de picos en vez de corte duro, evita distorsión en señales fuertes

### Orden real de los canales ADC

El hardware ESP32 entrega los canales en orden inverso al configurado: **CH7 (R) llega primero, CH6 (L) segundo**. El código ya lo tiene en cuenta al promediarlos.

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

```
idf.py set-target esp32
```

### 2. Compilar

```
idf.py fullclean
idf.py build
```

El `fullclean` es necesario cuando se modifica `sdkconfig.defaults`.

### 3. Flashear y monitorear

```
idf.py flash monitor
```

O por separado:

```
idf.py flash -p /dev/ttyUSB0
idf.py monitor -p /dev/ttyUSB0
```

En macOS el puerto suele ser `/dev/cu.usbserial-*`. En Windows `COM3`, `COM4`, etc.

---

## Calibración de la tasa ADC (paso importante)

La tasa real del ADC varía chip a chip. Si no se calibra, el audio puede sonar ligeramente acelerado o frenado (pitch incorrecto).

1. Flashear y conectar al monitor serie
2. Esperar ~5 segundos hasta ver: `Tasa ADC real: XXXXX Hz/canal`
3. Copiar ese valor y reemplazar `RESAMPLE_IN_RATE` en `bt_audio.c`
4. Recompilar y reflashear

```c
// bt_audio.c
#define RESAMPLE_IN_RATE   31909   // ← reemplazar con el valor del log
```

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
I (512) AUDIO_CAP: ADC continuo iniciado — modo MONO con oversampling x2
I (1162) BT_AUDIO: Iniciando discovery — pon auriculares en modo pairing
I (5512) AUDIO_CAP: Tasa ADC real: 31923 Hz/canal (objetivo: 44100)
I (16722) BT_AUDIO: A2DP conectado: aa:bb:cc:dd:ee:ff
I (16842) BT_AUDIO: Stream iniciado correctamente
I (16852) BT_AUDIO: Estado audio: STARTED
```

---

## Parámetros ajustables

En `audio_capture.c`:

| Parámetro | Valor | Descripción |
| --- | --- | --- |
| `GAIN` | `32` | Ganancia digital — subir a 48 si el volumen es bajo, bajar a 24 si distorsiona |
| `NOISE_GATE` | `40` | LSB — silencia señales por debajo de este umbral; bajar a 25 si corta voces suaves |
| `sample_freq_hz` | `78000` | Frecuencia de config del ADC — produce ~31,900 Hz/canal reales |

En `bt_audio.c`:

| Parámetro | Valor | Descripción |
| --- | --- | --- |
| `RESAMPLE_IN_RATE` | `31909` | Tasa real medida del ADC — **calibrar con el log** |
| `RESAMPLE_OUT_RATE` | `44100` | Tasa que espera el stack BT |
| `BT_DEVICE_NAME` | `"ESP32 Audio Bridge"` | Nombre visible por Bluetooth |

En `audio_capture.h`:

| Parámetro | Valor | Descripción |
| --- | --- | --- |
| `AUDIO_FRAME_SIZE` | `256` | Muestras por frame de captura |
| `AUDIO_RINGBUF_SIZE` | `FRAME_SIZE × 32 × 2 × sizeof(int16_t)` | Ring buffer estéreo (~32 kB) |

---

## Asignación de núcleos (dual-core)

| Tarea | Núcleo | Prioridad | Motivo |
| --- | --- | --- | --- |
| Stack Bluedroid / A2DP | Core 0 | alta (gestionada por IDF) | Reservado para BT |
| `audio_cap` (ADC DMA → ring buffer) | Core 1 | `MAX_PRIORITIES − 2` | No interfiere con BT |
| `app_main` / monitoreo | Core 1 | normal | Tarea secundaria |

> BT debe inicializarse **antes** que el ADC continuo para que `nvs_flash_init` no colisione con el ISR del DMA.

---

## Limitaciones conocidas

* **Output mono:** L y R del jack se mezclan en una sola señal. El output BT es mono duplicado en ambos auriculares.
* **Tasa de muestreo ADC limitada:** Con BT activo el ADC continuo produce ~31,900 Hz/canal. Se compensa con resampleo por interpolación lineal, que introduce una leve degradación de calidad. Calibrar `RESAMPLE_IN_RATE` con el log para minimizar el impacto.
* **Ruido del ADC interno:** El ADC del ESP32 tiene ~9.5 bits efectivos reales. El oversampling x2 mejora ~3 dB pero no elimina completamente el ruido de fondo. Para mayor fidelidad considerar un códec externo I2S como el **PCM1802** (~$4) o el **ES8388**.
* **Un solo dispositivo BT simultáneo:** A2DP Source solo admite una conexión activa.
* **Sin emparejamiento por nombre:** Se conecta al primer dispositivo A2DP encontrado. Para conectarse siempre a los mismos auriculares, guardar su MAC y filtrar en el callback GAP.
* **Latencia:** Entre 100 ms y 300 ms dependiendo del firmware de los auriculares.

---

## Solución de problemas

| Síntoma | Causa probable | Solución |
| --- | --- | --- |
| No encuentra auriculares | Auriculares no en modo pairing | Ponerlos en pairing antes de que termine el discovery (~10 s) |
| `Cache error` al arrancar | `sdkconfig.defaults` incompleto | Verificar `ADC_CONTINUOUS_ISR_IRAM_SAFE=y` y `SPI_FLASH_AUTO_SUSPEND=n`, luego `idf.py fullclean` |
| Audio acelerado / efecto ardilla | `RESAMPLE_IN_RATE` incorrecto | Medir la tasa real con el log `Tasa ADC real` y actualizar la constante |
| Audio muy bajo | `GAIN` insuficiente | Subir `GAIN` en `audio_capture.c` (probar con 48) |
| Audio distorsionado | `GAIN` excesivo | Reducir `GAIN` (probar con 24) |
| Ruido de moto sin señal | Decay del filtro IIR insuficiente | Verificar que `lp >>= 1` esté aplicado en la rama `raw == 0` |
| Ruido tipo lluvia con señal | Límite del ADC interno del ESP32 | Usar códec externo I2S (PCM1802 o ES8388) |
| Corta voces suaves | `NOISE_GATE` demasiado alto | Bajar `NOISE_GATE` de 40 a 25 en `audio_capture.c` |
| Watchdog IDLE en `audio_cap` | Tarea ADC sin ceder CPU | Verificar que se usa ADC continuo con ISR, no oneshot en loop |
| Error de compilación en ESP32-C3/S3 | Chip sin BT clásico | Usar ESP32 original únicamente |
| `CONFIG_BT_A2DP_ENABLE` no disponible | BT clásico desactivado | Ejecutar `idf.py menuconfig` y activar BT clásico |

---

## Licencia

MIT — libre para uso personal y comercial.

Entiendo la frustración. Seamos honestos: llegamos al límite de lo que se puede mejorar por software.

El problema es el **ADC interno del ESP32**. Tiene ~9.5 bits efectivos reales en vez de 12, y cuando Bluetooth está activo el ruido sube todavía más porque el stack BT genera interferencia eléctrica interna que se acopla al ADC. Ningún filtro, promedio ni noise gate va a eliminar ese ruido — solo lo puede disimular un poco.

La única solución real es sacar el ADC del ESP32 de la cadena y usar uno externo por I2S.

---

El módulo más simple y barato es el **PCM1802**:

- ~$3-5 en MercadoLibre o AliExpress
- Se conecta al ESP32 por 4 cables (I2S: BCK, LRCK, DATA, y opcionalmente SCKI)
- ADC de 24 bits reales, SNR de 100 dB vs los ~60 dB del ADC interno
- Recibe el jack 3.5mm directamente sin el circuito divisor de tensión que tenés ahora

El cambio en el código sería reemplazar `audio_capture.c` para usar el driver I2S del ESP-IDF en vez del ADC continuo. `bt_audio.c` no cambia nada.

¿Querés que te arme el circuito y el código para el PCM1802?