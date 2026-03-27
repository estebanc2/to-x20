# ESP32 BT Audio Bridge

Convierte un ESP32 en un adaptador de audio Bluetooth: recibe audio estéreo analógico desde un jack de 3.5 mm y lo transmite por Bluetooth A2DP a auriculares inalámbricos.

```
[Jack 3.5 mm estéreo]
    Canal L → GPIO34
    Canal R → GPIO35        →  [ADC ESP32]  →  [A2DP / SBC]  →  [Auriculares BT]
    GND     → GND
```

---

## Requisitos

### Hardware

| Componente | Detalle |
|---|---|
| ESP32 (cualquier variante) | WROOM-32, WROVER, DevKit, etc. |
| Cable / conector jack 3.5 mm | TRS o TRRS estéreo |
| 2× resistencia 10 kΩ | Divisor de tensión por canal |
| 2× condensador electrolítico 10 µF | Bloqueo de DC por canal |
| Protoboard + cables | Para el circuito de acondicionamiento |
| Auriculares Bluetooth | Compatibles con A2DP (la mayoría lo son) |

### Software

| Herramienta | Versión mínima | Enlace |
|---|---|---|
| ESP-IDF | **v5.1** | https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/ |
| VSCode | cualquier reciente | https://code.visualstudio.com |
| Espressif IDF Extension | v1.7+ | Marketplace de VSCode |

> ⚠️ **No uses ESP-IDF v4.x.** Las APIs `esp_adc/adc_oneshot.h` y el sistema de calibración ADC moderno requieren v5.0 o superior.

---

## Estructura del proyecto

```
esp32_bt_audio/
├── CMakeLists.txt          # CMake raíz
├── sdkconfig.defaults      # Configuración Kconfig predefinida
└── main/
    ├── CMakeLists.txt      # Componente principal
    ├── main.c              # Punto de entrada (app_main)
    ├── audio_capture.c     # Tarea ADC + ring buffer
    ├── audio_capture.h
    ├── bt_audio.c          # Stack Bluedroid + A2DP Source
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

> El divisor centra la señal en **1.65 V** (mitad de 3.3 V), que es el punto de reposo ideal para el ADC.

---

## Instalación y configuración

### 1. Clonar / crear el proyecto

Copia los archivos en una carpeta nueva o usa la extensión de VSCode:

```
Ctrl+Shift+P → ESP-IDF: New Project
```

### 2. Seleccionar el target

```bash
idf.py set-target esp32
```

### 3. Aplicar configuración predefinida

El archivo `sdkconfig.defaults` ya habilita Bluetooth clásico y A2DP. Verifica con:

```bash
idf.py menuconfig
```

Rutas relevantes en menuconfig:

```
Component config → Bluetooth → [*] Bluetooth
                             → [*] Bluedroid Enable
                             → [*] Classic Bluetooth
                             → [*] A2DP

Component config → Driver → ADC → calibration
```

### 4. Compilar

```bash
idf.py build
```

### 5. Flashear y monitorear

```bash
idf.py flash monitor
```

O por separado:

```bash
idf.py flash -p /dev/ttyUSB0
idf.py monitor -p /dev/ttyUSB0
```

En Windows el puerto suele ser `COM3`, `COM4`, etc.

---

## Uso

1. Conecta el jack 3.5 mm a la salida de auriculares de tu fuente de audio.
2. Alimenta el ESP32 (USB o 3.3 V externo).
3. **Pon tus auriculares Bluetooth en modo pairing.**
4. El ESP32 iniciará un escaneo activo y se conectará automáticamente al primer dispositivo A2DP que encuentre.
5. La salida serie (115200 baud) muestra el estado:

```
I (312) MAIN: === ESP32 BT Audio Bridge ===
I (318) MAIN: IDF version: v5.1.2
I (420) BT_AUDIO: Iniciando discovery — pon auriculares en modo pairing
I (5821) BT_AUDIO: Dispositivo encontrado — intentando A2DP...
I (6103) BT_AUDIO: A2DP conectado: aa:bb:cc:dd:ee:ff
I (6110) BT_AUDIO: Estado audio: STARTED
```

---

## Parámetros ajustables

Todos en `audio_capture.h`:

| Parámetro | Valor por defecto | Descripción |
|---|---|---|
| `AUDIO_ADC_LEFT_CH` | `ADC_CHANNEL_6` (GPIO34) | Canal ADC izquierdo |
| `AUDIO_ADC_RIGHT_CH` | `ADC_CHANNEL_7` (GPIO35) | Canal ADC derecho |
| `AUDIO_SAMPLE_RATE` | `44100` | Frecuencia de muestreo (Hz) |
| `AUDIO_FRAME_SIZE` | `512` | Muestras por frame |
| `AUDIO_OVERSAMPLE` | `4` | Lecturas promediadas por muestra |
| `AUDIO_RINGBUF_SIZE` | `FRAME_SIZE × 8 × 2` | Tamaño del buffer circular (bytes) |

En `bt_audio.h`:

| Parámetro | Valor por defecto | Descripción |
|---|---|---|
| `BT_DEVICE_NAME` | `"ESP32 Audio Bridge"` | Nombre visible por Bluetooth |

---

## Asignación de núcleos (dual-core)

| Tarea | Núcleo | Prioridad | Motivo |
|---|---|---|---|
| Stack Bluedroid / A2DP | Core 0 | alta (gestionada por IDF) | Reservado para BT |
| `audio_cap` (ADC → ring buffer) | Core 1 | `MAX_PRIORITIES − 2` | No interfiere con BT |
| `app_main` / monitoreo | Core 1 | normal | Tarea secundaria |

---

## Limitaciones conocidas

- **Calidad de audio del ADC interno:** El ADC del ESP32 tiene ruido inherente (~50–100 LSB). Es suficiente para uso casual, pero no es de calidad audiófila. Para mayor fidelidad considera un códec externo I2S como el PCM1802 o el ES8388.
- **Un solo dispositivo BT simultáneo:** El perfil A2DP Source del ESP32 solo admite una conexión activa.
- **Sin re-emparejamiento automático por nombre:** El código se conecta al primer dispositivo A2DP encontrado en el escaneo. Si quieres conectarte siempre a los mismos auriculares, guarda su dirección MAC y filtra en el callback GAP.
- **Latencia:** Esperada entre 100 ms y 300 ms dependiendo del codec SBC negociado y del firmware de los auriculares.

---

## Solución de problemas

| Síntoma | Causa probable | Solución |
|---|---|---|
| No encuentra auriculares | Auriculares no en modo pairing | Mantenlos en modo emparejamiento durante el arranque |
| Audio muy bajo | Ganancia insuficiente | Sube el factor en `audio_capture.c` (línea `* 16`) |
| Audio distorsionado / clipping | Ganancia excesiva o señal de entrada muy alta | Reduce el factor de ganancia |
| ADC lee siempre ~2048 | Falta el condensador de bloqueo DC | Verifica C1/C2 en el circuito |
| Error de compilación en `adc_oneshot.h` | ESP-IDF < v5.0 | Actualiza a v5.1+ |
| `CONFIG_BT_A2DP_ENABLE` no disponible | BT clásico desactivado en sdkconfig | Ejecuta `idf.py menuconfig` y actívalo |

---

## Licencia

MIT — libre para uso personal y comercial.
