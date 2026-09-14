| Supported Targets | ESP32-S2 |
| ----------------- | -------- |

# multitarea — App 4 (Clase 4)

Firmware base de la **App 4**. Cuatro tareas de **FreeRTOS** se reparten la **ESP32-S2-Kaluga-1**, cada una con su prioridad y su forma de esperar, coordinadas con **todos** los mecanismos de la Sección 4: **cola**, **mutex**, **semáforo binario**, **notificación de tarea** y **temporizador software**.

```
                   (sensor interno de temperatura)
                                │
    [sensor  p6] ──── cola ────▶[proceso p5]───┐
     periódica              muestra_t          │  mutex
                                               ▼
                                        ┌─────────────┐
    [tactil  p9] ──notificación────────▶│   estado    │
     bloqueada en la cola de la ISR     │  compartido │
                                        └─────────────┘
                                               │  mutex
    [interfaz p3] ◀──────────────────────────── ┘
     periódica: LED + panel por consola

    [timer software] ──▶ informe de tareas cada 10 s
```

Lo que hay que ver funcionando:

- cada tarea se escribe **como si fuera el único programa** de la placa;
- ninguna consulta a las demás: todas están **BLOQUEADAS** casi siempre, que es como una tarea dice "no me des CPU hasta que haya algo para mí";
- el **dato** viaja por una **cola** (copia, productor–consumidor) y el **estado** se comparte con **mutex** (exclusión mutua);
- el informe periódico enseña el **estado** y la **pila real** de cada tarea.

## Hardware

- **ESP-LyraP-TouchA** conectada al **Touch FPC Connector** de la Kaluga-1 con el cable plano de 20 pines.
- **Microinterruptores T1–T14** de la cara inferior en **OFF**.
- **LED RGB direccionable** en **GPIO45**. ⚠️ **Coloca el jumper RGB**: sin él, el LED no recibe señal.
- **Sensor de temperatura:** el **interno del ESP32-S2**. No hace falta nada más. Mide la temperatura **del chip**, no la del aire: sube varios grados en cuanto la CPU trabaja.
- Target ESP-IDF: **`esp32s2`** (touch sensor **V2**).

## Las cuatro tareas

| Tarea | Prio | Espera en | Trabajo | Por qué esa prioridad |
|---|:-:|---|---|---|
| **tactil** | 9 | Cola de la ISR | Cambia modo, reinicia min/max | Responde a una persona: plazo corto |
| **sensor** | 6 | `xTaskDelayUntil` | Lee la temperatura cada 500 ms | Periódica y con plazo fijo |
| **proceso** | 5 | Cola de muestras | Media móvil, min/max | Consume lo que produce el sensor |
| **interfaz** | 3 | Notificación o plazo | LED + panel por consola | Si se retrasa 50 ms, nadie lo nota |

> En FreeRTOS, **número mayor = prioridad mayor** (al revés que en Linux). El criterio para asignarlas no es "qué es más importante" sino **qué tiene el plazo más corto**: es la idea de *rate-monotonic*.

## Mapa de botones

| Serigrafía | Canal | Acción |
|---|:-:|---|
| **PLAY** | T2 | Cambia de modo: NORMAL → RAPIDO → SILENCIO |
| **VOL_UP** | T1 | Reinicia mínimo y máximo |
| **VOL_DOWN** | T4 | Fuerza el informe de tareas ahora mismo |

| Modo | Qué cambia |
|---|---|
| **NORMAL** | Sensor a 500 ms, LED como termómetro |
| **RAPIDO** | Sensor a 250 ms: se ve subir el número de muestras |
| **SILENCIO** | LED apagado, todo lo demás sigue |

## Colores del LED

| Color | Significa |
|---|---|
| **Ámbar** | Arrancando y calibrando |
| **Azul** | Media por debajo de 30 °C |
| **Verde** | Entre 30 y 40 °C |
| **Rojo** | Por encima de 40 °C |
| **Apagado** | Modo SILENCIO |

## Estructura

```
multitarea/
├── CMakeLists.txt
├── sdkconfig.defaults        Tick de 1 ms, estadísticas de tareas, canario de pila
├── main/
│   ├── CMakeLists.txt
│   ├── idf_component.yml     Declara la dependencia espressif/led_strip
│   └── multitarea_main.c
└── README.md   (este archivo)
```

## Cómo compilar, flashear y monitorear

```bash
idf.py set-target esp32s2
idf.py build
idf.py -p /dev/cu.usbserial-XXXX flash monitor
```

- Sustituye `/dev/cu.usbserial-XXXX` por tu puerto (`ls /dev/cu.*` en macOS).
- Conecta el cable al puerto **UART** de la placa.
- Salir del monitor: `Ctrl + ]`.

> ⚠️ **No toques la placa durante el arranque.** Si el `benchmark` inicial se mide con el dedo apoyado, ese botón no se activará hasta el siguiente reset.

## Salida esperada

```
I (301) multitarea: === multitarea: cuatro tareas de FreeRTOS en la Kaluga-1 ===
I (302) multitarea: PLAY = cambia de modo | VOL_UP = reinicia min/max | VOL_DOWN = informe ya
I (335) multitarea: Calibrando... NO toques la placa durante el arranque
I (352) multitarea:   PLAY     T2   benchmark=40887  umbral=817
I (353) multitarea:   VOL_UP   T1   benchmark=39954  umbral=799
I (354) multitarea:   VOL_DOWN T4   benchmark=41102  umbral=822
I (356) multitarea: Arranque completo. app_main termina; el planificador sigue.
I (357) multitarea: [sensor] arranca, periodo 500 ms
I (861) multitarea: [interfaz] 42,18 C  media 42,18  min 42,18  max 42,18  | n=1  toques=0  modo=NORMAL
I (1362) multitarea: [interfaz] 42,50 C  media 42,34  min 42,18  max 42,50  | n=2  toques=0  modo=NORMAL
I (3204) multitarea: [tactil] PLAY (T2) -> modo RAPIDO | latencia ISR->tarea 34 us
I (10362) multitarea: --- Informe de tareas (cada 10 s) ------------------------
I (10363) multitarea:   tarea      prio estado      pila libre
I (10364) multitarea:   tactil     9    BLOQUEADA   2216 B
I (10365) multitarea:   sensor     6    BLOQUEADA   2160 B
I (10366) multitarea:   proceso    5    BLOQUEADA   2408 B
I (10367) multitarea:   interfaz   3    BLOQUEADA   2952 B
I (10368) multitarea:   cola de muestras: 0/8 llenas, 0 perdidas
I (10369) multitarea:   jitter maximo del sensor: 312 us  (periodo 500 ms, tick 1 ms)
I (10370) multitarea:   heap libre: 218412 B  (minimo historico 216980 B)
```

**Las cuatro tareas aparecen BLOQUEADAS.** No es un fallo: es el objetivo. En un diseño con RTOS bien hecho, la CPU se pasa la vida en la tarea IDLE y las tareas de la aplicación solo despiertan cuando tienen algo que hacer. Ese es el requisito previo para todo lo de la Clase 3: solo se puede dormir un chip cuyas tareas están bloqueadas.

## Qué mecanismo se usa dónde, y por qué

| Mecanismo | Dónde | Por qué ese y no otro |
|---|---|---|
| **Cola** | `sensor → proceso` | Hay que transportar **un dato**, y amortiguar ráfagas |
| **Cola** | `ISR → tactil` | La ISR entrega trabajo a una tarea: patrón diferido |
| **Mutex** | Acceso a `s_estado` | Tres tareas escriben/leen la misma estructura |
| **Semáforo binario** | `interfaz → sensor` | Una **señal** de una sola vez: "ya estoy lista" |
| **Notificación** | `proceso → interfaz` | Un emisor, un receptor: es lo más barato del kernel |
| **Timer software** | Informe cada 10 s | Trabajo periódico y ligero, sin gastar una tarea |

> Un **mutex** no es un semáforo binario con otro nombre: el mutex tiene **dueño** y **herencia de prioridad**, y por eso es el único correcto para proteger un recurso. Un semáforo binario no tiene dueño, y por eso es el correcto para **señalizar**. Ver [../../detalle-cola-semaforo-o-mutex.md](../../detalle-cola-semaforo-o-mutex.md).

## Detalles de diseño que conviene mirar en el código

- **Las secciones críticas son cortas.** Se toma el mutex, se copia el estado, se suelta, y solo **después** se formatea e imprime. Retener un mutex mientras se escribe por la UART es el error clásico: bloquea a todo el que quiera el mismo dato durante milisegundos.
- **La cola no bloquea al productor.** `xQueueSend(..., 0)` prefiere **perder una muestra** a retrasar la tarea periódica, y **cuenta** lo que pierde para poder justificar la decisión.
- **El periodo se mide con `xTaskDelayUntil`,** que cuenta desde el despertar anterior. Con `vTaskDelay()` el periodo se iría deslizando a razón de lo que tarde el cuerpo del bucle.
- **El jitter se mide.** Es la desviación real respecto del periodo teórico, y es la única prueba de que el sistema cumple sus plazos.
- **Las pilas se ajustan con datos.** El `pila libre` del informe es `uxTaskGetStackHighWaterMark()`: lo que le **sobró**, no lo que gastó.
- **No hay ni un `float` fuera del sensor.** El Xtensa LX7 del ESP32-S2 no tiene FPU: la lectura se convierte a centésimas enteras una sola vez. Ver [../../../clase-2/detalle-punto-fijo-y-float.md](../../../clase-2/detalle-punto-fijo-y-float.md).

## Por qué el `sdkconfig.defaults`

| Opción | Para qué |
|---|---|
| `CONFIG_FREERTOS_HZ=1000` | Tick de **1 ms** en vez de 10. La resolución temporal de todo el sistema es el tick: con 100 Hz, el jitter del sensor no bajaría de 10 ms |
| `CONFIG_FREERTOS_USE_TRACE_FACILITY=y` | Habilita `uxTaskGetSystemState()` y `vTaskList()` |
| `CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS=y` | Idem |
| `CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY=y` | Avisa si una tarea desborda su pila, en vez de reiniciar sin explicación |

Subir el tick cuesta **más interrupciones de reloj** (y algo de consumo): es el compromiso clásico entre resolución y sobrecarga. Pruébalo en los dos valores y compara el jitter del informe.

## Cosas para probar

| Experimento | Qué observar |
|---|---|
| `CONFIG_FREERTOS_HZ=100` en `menuconfig` | El jitter máximo salta a ~10 ms: **el tick es el suelo** |
| Poner `PRIO_INTERFAZ` a 9, por encima del sensor | El panel manda y el sensor pierde plazos: la prioridad **es** el diseño |
| Poner las cuatro tareas a la misma prioridad | Reparto por turnos (*time slicing*): funciona, pero ya no hay garantías |
| `COLA_MUESTRAS_LARGO = 1` y un `vTaskDelay(2000)` en `proceso` | Aparecen muestras perdidas: la cola es un **amortiguador**, y tiene fondo |
| Quitar el mutex de `tarea_interfaz` | Panel con datos incoherentes: media de una muestra, mínimo de otra |
| Bajar `PILA_INTERFAZ` a 1024 | `***ERROR*** A stack overflow in task interfaz has been detected` |
| Meter un `ESP_LOGI` dentro de la sección crítica | El jitter empeora: la sección crítica se alarga |
| Un `vTaskDelay()` dentro del callback del timer | Se retrasan **todos** los temporizadores del sistema |

## Solución de problemas

| Síntoma | Solución |
|---|---|
| Ningún botón responde | Microinterruptores **T1–T14 en OFF**; cable FPC bien insertado |
| Un canal siempre activo | Se calibró con el dedo encima: reiniciar sin tocar |
| Despierta solo, sin tocar | Subir `UMBRAL_RATIO` a `0.03f`–`0.05f` |
| No responde nunca al toque | Bajar `UMBRAL_RATIO` a `0.01f`; recalibrar sin tocar |
| El LED no enciende | Colocar el **jumper RGB**; verificar GPIO45 |
| `A stack overflow in task X` | Subir la pila de esa tarea; mirar su `pila libre` en el informe |
| `lectura fuera de rango` | La temperatura del chip se salió de −10…80 °C; ajustar el rango |
| `assert failed: xQueueGenericSend` desde la ISR | Se llamó a la versión sin `FromISR` en contexto de interrupción |
| No compila (`touch_sens.h`) | Requiere **ESP-IDF ≥ v5.4**; se probó con **v6.0.2** |
| `Failed to connect` al flashear | Secuencia **BOOT + RST** (modo bootloader) |

## Referencias

- [../../detalles/freertos-en-esp-idf.md](../../detalles/freertos-en-esp-idf.md)
- [../../detalles/arquitecturas-de-firmware.md](../../detalles/arquitecturas-de-firmware.md)
- [../../detalle-cola-semaforo-o-mutex.md](../../detalle-cola-semaforo-o-mutex.md)
- [../../detalle-latencia-y-peor-caso.md](../../detalle-latencia-y-peor-caso.md)
- [../../../clase-2/detalles/interrupciones-y-datos-compartidos.md](../../../clase-2/detalles/interrupciones-y-datos-compartidos.md)
- [../../../clase-2/detalles/touch-capacitivo-esp32s2.md](../../../clase-2/detalles/touch-capacitivo-esp32s2.md)
- Barry, R. *Mastering the FreeRTOS Real-Time Kernel*, cap. 3–5.
- Espressif, *ESP-IDF Programming Guide — FreeRTOS (IDF)* y *Temperature Sensor*.
- Guía de conexión e instalación: [../../../bibliografia/Guia-Kaluga-1-macOS-ESP-IDF-VSCode.md](../../../bibliografia/Guia-Kaluga-1-macOS-ESP-IDF-VSCode.md)
