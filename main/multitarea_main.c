/*
 * multitarea — Clase 4 (Sistemas Embebidos, UCU 2026)
 *
 * Base de la App 4: cuatro tareas de FreeRTOS repartiéndose la placa, cada una
 * con su prioridad y su forma de esperar, coordinadas con TODOS los mecanismos
 * de la Sección 4: cola, mutex, semáforo binario, notificación de tarea y
 * temporizador software.
 *
 *                     (sensor interno de temperatura)
 *                                  │
 *      [sensor  p6] ──── cola ────▶[proceso p5]───┐
 *       periódica              muestra_t          │  mutex
 *                                                 ▼
 *                                          ┌─────────────┐
 *      [tactil  p9] ──notificación────────▶│   estado    │
 *       bloqueada en semáforo del ISR      │  compartido │
 *                                          └─────────────┘
 *                                                 │  mutex
 *      [interfaz p3] ◀──────────────────────────── ┘
 *       periódica: LED + panel por consola
 *
 *      [timer software] ──▶ informe de tareas cada 10 s
 *
 * Lo que hay que ver:
 *   - cada tarea se escribe como si fuera el único programa de la placa;
 *   - ninguna consulta a las demás: todas están BLOQUEADAS casi siempre;
 *   - el dato viaja por la COLA (copia, productor-consumidor) y el estado se
 *     comparte con MUTEX (exclusión mutua, con herencia de prioridad);
 *   - el informe periódico enseña el estado y la pila real de cada tarea.
 *
 * Hardware:
 *   - ESP-LyraP-TouchA en el Touch FPC Connector (cable de 20 pines).
 *   - Microinterruptores T1-T14 de la cara inferior en OFF.
 *   - LED RGB direccionable en GPIO45 (requiere el JUMPER RGB colocado).
 *   - Sensor de temperatura: el INTERNO del ESP32-S2, no necesita nada más.
 *   - Target: esp32s2 (touch sensor V2).
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "driver/temperature_sensor.h"
#include "driver/touch_sens.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "led_strip.h"

#define RGB_LED_GPIO            45
#define RGB_LED_COUNT           1

/* --- Prioridades ---------------------------------------------------------
 *
 * En FreeRTOS, número MAYOR = prioridad MAYOR (al revés que en Linux). El
 * criterio no es "qué es más importante" sino "qué tiene el plazo más corto":
 * la tarea que responde a una persona va arriba; la que dibuja, abajo.
 *
 *      0                        IDLE (la del sistema)
 *      1..24                    disponibles para la aplicación
 *      configMAX_PRIORITIES-1   = 24 en ESP-IDF por defecto
 */
#define PRIO_TACTIL             9    /* responde a un dedo: plazo corto      */
#define PRIO_SENSOR             6    /* periódica, 500 ms                    */
#define PRIO_PROCESO            5    /* consume lo que produce el sensor     */
#define PRIO_INTERFAZ           3    /* refresco visual: puede esperar       */
#define PRIO_CARGA              2    //no tiene ningún plazo funcional, porque su único objetivo es generar carga térmica 
#define PILA_CARGA              2048
#define PRIO_REGISTRO           4

/* --- Tamaños de pila, en BYTES (ESP-IDF; en FreeRTOS puro son palabras) ---
 *
 * No son mágicos: se ajustan con uxTaskGetStackHighWaterMark(), que es lo que
 * imprime el informe periódico. Se empieza generoso y se baja con datos. */
#define PILA_TACTIL             3072
#define PILA_SENSOR             3072
#define PILA_PROCESO            3072
#define PILA_INTERFAZ           4096
#define PILA_REGISTRO           3072

/* --- Periodos y capacidades --------------------------------------------- */
#define SENSOR_PERIODO_MS       500
#define INTERFAZ_PERIODO_MS     500
#define INFORME_PERIODO_MS      10000

#define UMBRAL_INICIAL_X100     4500
#define UMBRAL_ALTO_X100        5000
#define UMBRAL_BAJO_X100        4000

#define PARPADEO_ALARMA_MS      250 //para prender y apagar, cada estado dura la mitad
#define EVENTOS_MAX              5

#define COLA_MUESTRAS_LARGO     8     /* amortigua ráfagas: 4 s de holgura   */
#define COLA_TOQUES_LARGO       16
#define VENTANA_MEDIA           8     /* muestras de la media móvil          */

#define COLA_EVENTOS_LARGO      16

#define HISTERESIS_X100          200


/* --- Calibración del táctil (igual que en las Clases 2 y 3) -------------- */
#define BARRIDOS_CALIBRACION    3
#define UMBRAL_RATIO            0.02f
#define UMBRAL_INICIAL          2000

static const char *TAG = "multitarea";

/* ======================================================================== */
/*  Tipos                                                                    */
/* ======================================================================== */

/* Lo que viaja por la cola. Se envía POR COPIA: la tarea productora puede
 * reutilizar su variable en cuanto xQueueSend() retorna, y no hay ningún
 * puntero compartido que pueda quedar colgando. */
typedef struct {
    int32_t  centigrados_x100;   /* 2537 = 25,37 °C (entero: el S2 no tiene FPU) */
    int64_t  marca_us;
    uint32_t secuencia;
} muestra_t;

/* Lo que la ISR del táctil entrega a su tarea. Un tipo propio, y no el del
 * sensor: dos flujos distintos, dos contratos distintos. */
typedef struct {
    int     canal;
    int64_t marca_us;
} toque_t;

typedef enum {
    MODO_NORMAL = 0,
    MODO_RAPIDO,
    MODO_SILENCIO,
    MODO_N
} modo_t;

static const char *NOMBRE_MODO[MODO_N] = { "NORMAL", "RAPIDO", "SILENCIO" };

/* Estado compartido: lo escriben "proceso" y "tactil", lo lee "interfaz".
 * Tres tareas distintas tocando la misma estructura es EXACTAMENTE el caso
 * para el que existe un mutex. */
typedef struct {
    int32_t  ultima_x100;
    int32_t  media_x100;
    int32_t  minima_x100;
    int32_t  maxima_x100;
    int32_t  umbral_x100;
    uint32_t muestras;
    uint32_t toques;
    modo_t   modo;
    bool     carga_activa;
    bool     alarma;
    uint32_t alarmas;
} estado_t;

typedef enum {
    EVENTO_TOQUE,
    EVENTO_CARGA,
    EVENTO_UMBRAL,
    EVENTO_ALARMA
} tipo_evento_t;

typedef struct {
    tipo_evento_t tipo;
    int64_t marca_us;
    int32_t valor;
} evento_t;

/* ======================================================================== */
/*  Objetos del kernel                                                       */
/* ======================================================================== */

static QueueHandle_t     s_cola_muestras;  /* sensor  -> proceso  (datos)   */
static QueueHandle_t     s_cola_toques;    /* ISR     -> tactil   (datos)   */
static SemaphoreHandle_t s_mutex_estado;   /* protege s_estado              */
static SemaphoreHandle_t s_sem_arranque;   /* aviso de "ya estoy listo"     */
static TimerHandle_t     s_timer_informe;  /* temporizador software         */
static TaskHandle_t      s_tarea_interfaz; /* destino de las notificaciones */
static TaskHandle_t      s_tarea_carga;    // handle de la carga 
static QueueHandle_t     s_cola_eventos;
static TimerHandle_t s_timer_parpadeo;

static estado_t s_estado = {
    .minima_x100 = INT32_MAX,
    .maxima_x100 = INT32_MIN,
    .umbral_x100 = UMBRAL_INICIAL_X100,
    .carga_activa = false,
    .alarma = false,
    .alarmas = 0,
};

static led_strip_handle_t s_led;
static temperature_sensor_handle_t s_tsens;
static touch_sensor_handle_t s_touch;

static void callback_parpadeo(TimerHandle_t timer)
{
    s_parpadeo_alarma = !s_parpadeo_alarma;
    xTaskNotifyGive(s_tarea_interfaz);
}

/* Contadores de diagnóstico. Los escribe una sola tarea cada uno. */
static uint32_t s_perdidas_cola;    /* muestras que no cupieron en la cola  */
static uint32_t s_jitter_max_us;    /* peor desviación del periodo del sensor */

/* ======================================================================== */
/*  Botones táctiles                                                         */
/* ======================================================================== */

typedef struct {
    int canal;
    const char *nombre;
} boton_t;

static const boton_t BOTONES[] = {
    { 2, "PLAY"     },   /* cambia de modo            */
    { 1, "VOL_UP"   },   /* reinicia mínimos y máximos */
    { 4, "VOL_DOWN" },   /* fuerza un informe ya       */
};
#define BOTONES_N   (sizeof(BOTONES) / sizeof(BOTONES[0]))

static touch_channel_handle_t s_canales[BOTONES_N];

/* ======================================================================== */
/*  Utilidades                                                               */
/* ======================================================================== */

static void led(uint8_t r, uint8_t g, uint8_t b)
{
    ESP_ERROR_CHECK(led_strip_set_pixel(s_led, 0, r, g, b));
    ESP_ERROR_CHECK(led_strip_refresh(s_led));
}

/* Formatea centésimas de grado sin tocar un solo float: el Xtensa LX7 del
 * ESP32-S2 no tiene FPU. Ver clase-2/detalle-punto-fijo-y-float.md. */
static void formatear_temp(char *dst, size_t n, int32_t x100)
{
    const char *signo = (x100 < 0) ? "-" : "";
    const int32_t abs_x100 = (x100 < 0) ? -x100 : x100;

    snprintf(dst, n, "%s%" PRId32 ",%02" PRId32,
             signo, abs_x100 / 100, abs_x100 % 100);
}

static void enviar_evento(tipo_evento_t tipo, int32_t valor)
{
    const evento_t evento = {
        .tipo = tipo,
        .marca_us = esp_timer_get_time(),
        .valor = valor,
    };

    xQueueSend(s_cola_eventos, &evento, 0);
}

static volatile bool s_parpadeo_alarma = false;

/* ======================================================================== */
/*  ISR del táctil                                                           */
/* ======================================================================== */

/* Contexto de interrupción: SOLO encola. Todo lo demás ocurre en la tarea.
 * Es el patrón "deferred interrupt processing" de la Clase 2, y en la
 * Clase 4 se ve por qué es el patrón natural de un RTOS: la ISR entrega el
 * trabajo a una tarea que el planificador puede colocar donde corresponda. */
static bool IRAM_ATTR al_tocar(touch_sensor_handle_t sensor,
                               const touch_active_event_data_t *ev,
                               void *ctx)
{
    BaseType_t hp_task_woken = pdFALSE;
    const toque_t t = { .canal = ev->chan_id,
                        .marca_us = esp_timer_get_time() };

    xQueueSendFromISR(s_cola_toques, &t, &hp_task_woken);

    /* Devolver true equivale a portYIELD_FROM_ISR(): al salir de la ISR el
     * planificador entra de inmediato, en vez de esperar al siguiente tick.
     * Sin esto, la tarea de prioridad 9 se quedaría esperando hasta 1 ms. */
    return hp_task_woken == pdTRUE;
}

/* ======================================================================== */
/*  TAREA 1 — sensor:  periódica, produce muestras                           */
/* ======================================================================== */

/* Prioridad 6. Lee el sensor interno de temperatura cada SENSOR_PERIODO_MS y
 * mete el resultado en la cola. No sabe quién lo consume ni le importa: ese
 * desacoplo es la mitad del valor de una cola. */
static void tarea_sensor(void *arg)
{
    TickType_t anterior = xTaskGetTickCount();
    uint32_t secuencia = 0;
    int64_t esperado_us = esp_timer_get_time();

    /* Espera a que la interfaz esté lista antes de empezar a producir. Un
     * semáforo binario usado como "señal de una sola vez". */
    xSemaphoreTake(s_sem_arranque, portMAX_DELAY);
    ESP_LOGI(TAG, "[sensor] arranca, periodo %d ms", SENSOR_PERIODO_MS);

    while (1) {
        /* El driver devuelve grados en float; lo pasamos a centésimas
         * enteras AQUI MISMO y el resto del programa no vuelve a ver un
         * float. Una conversión, y no una por cada cuenta. */
        float celsius = 0.0f;
        if (temperature_sensor_get_celsius(s_tsens, &celsius) != ESP_OK) {
            /* Fuera del rango configurado. Se avisa y se sigue: una tarea
             * periódica no se muere porque una lectura salga mal. */
            ESP_LOGW(TAG, "[sensor] lectura fuera de rango, se descarta");
            xTaskDelayUntil(&anterior, pdMS_TO_TICKS(SENSOR_PERIODO_MS));
            continue;
        }

        const muestra_t m = {
            .centigrados_x100 = (int32_t)(celsius * 100.0f),
            .marca_us = esp_timer_get_time(),
            .secuencia = secuencia++,
        };

        /* Con xTicksToWait = 0 la tarea NO se bloquea si la cola está llena:
         * prefiere perder una muestra a retrasarse. Es una decisión de
         * diseño, y se cuenta lo que se pierde para poder justificarla. */
        if (xQueueSend(s_cola_muestras, &m, 0) != pdTRUE) {
            s_perdidas_cola++;
            ESP_LOGW(TAG, "[sensor] cola llena, muestra %" PRIu32 " perdida",
                     m.secuencia);
        }

        /* Jitter: cuánto se desvía el despertar real del periodo teórico. Es
         * la medida de si el sistema cumple sus plazos. */
        esperado_us += (int64_t)SENSOR_PERIODO_MS * 1000;
        const int64_t desvio = m.marca_us - esperado_us;
        const uint32_t abs_desvio = (uint32_t)((desvio < 0) ? -desvio : desvio);
        if (abs_desvio > s_jitter_max_us) {
            s_jitter_max_us = abs_desvio;
        }

        /* xTaskDelayUntil mide el periodo desde el despertar ANTERIOR, no
         * desde ahora. vTaskDelay() acumularía el tiempo del cuerpo del
         * bucle y el periodo se iría deslizando. */
        /* El modo lo escribe la tarea del táctil: leerlo sin el mutex sería
         * una carrera, aunque sea un solo enum. Barato y correcto es mejor
         * que barato. */
        xSemaphoreTake(s_mutex_estado, portMAX_DELAY);
        const modo_t modo = s_estado.modo;
        xSemaphoreGive(s_mutex_estado);

        const uint32_t periodo = (modo == MODO_RAPIDO) ? SENSOR_PERIODO_MS / 2
                                                       : SENSOR_PERIODO_MS;
        xTaskDelayUntil(&anterior, pdMS_TO_TICKS(periodo));
    }
}

/* ======================================================================== */
/*  TAREA 2 — proceso:  consume la cola y actualiza el estado                 */
/* ======================================================================== */

/* Prioridad 5, justo por debajo del sensor: si ambas están listas, produce
 * primero y consume después. Bloqueada en la cola el 99,9 % del tiempo. */
static void tarea_proceso(void *arg)
{
    int32_t ventana[VENTANA_MEDIA] = { 0 };
    size_t n = 0, i = 0;
    muestra_t m;

    while (xQueueReceive(s_cola_muestras, &m, portMAX_DELAY) == pdTRUE) {
        /* Media móvil, en enteros. */
        ventana[i] = m.centigrados_x100;
        i = (i + 1) % VENTANA_MEDIA;
        if (n < VENTANA_MEDIA) { n++; }

        int64_t suma = 0;
        for (size_t k = 0; k < n; k++) {
            suma += ventana[k];
        }

        /* SECCION CRITICA. Se toma el mutex, se escribe y se suelta
         * inmediatamente: dentro no hay ni un ESP_LOGI ni una espera. Cuanto
         * más corta, menos bloquea a las otras tareas. */
        xSemaphoreTake(s_mutex_estado, portMAX_DELAY);
        s_estado.ultima_x100 = m.centigrados_x100;
        
        const int32_t media = (int32_t)(suma / (int64_t)n);
        s_estado.media_x100 = media;
        
        if (m.centigrados_x100 < s_estado.minima_x100) {
            s_estado.minima_x100 = m.centigrados_x100;
        }
        if (m.centigrados_x100 > s_estado.maxima_x100) {
            s_estado.maxima_x100 = m.centigrados_x100;
        }
        s_estado.muestras++;

        if (!s_estado.alarma && media > s_estado.umbral_x100) {
            s_estado.alarma = true;
            s_estado.alarmas++;

            xTimerStart(s_timer_parpadeo, 0);

            enviar_evento(EVENTO_ALARMA, 1);

        } else if (s_estado.alarma &&
           media < s_estado.umbral_x100 - HISTERESIS_X100) {
            s_estado.alarma = false;

            xTimerStop(s_timer_parpadeo, 0);

            enviar_evento(EVENTO_ALARMA, 0);
        }


        xSemaphoreGive(s_mutex_estado);

        /* Avisa a la interfaz de que hay dato nuevo. Una NOTIFICACION DIRECTA
         * es lo más barato del kernel: no hay objeto que crear, el contador
         * vive dentro del TCB de la tarea destino. Sirve porque solo hay un
         * emisor y un receptor. */
        xTaskNotifyGive(s_tarea_interfaz);
    }
}


/* ======================================================================== */
/*  TAREA 3 — tactil:  atiende los toques diferidos de la ISR                */
/* ======================================================================== */

/* Prioridad 9, la más alta de la aplicación: responde a una persona, y una
 * persona nota 100 ms. Se pasa la vida bloqueada en su cola. */
static void tarea_tactil(void *arg)
{
    toque_t ev;

    while (xQueueReceive(s_cola_toques, &ev, portMAX_DELAY) == pdTRUE) {
        const int canal = ev.canal;
        enviar_evento(EVENTO_TOQUE, canal);

        const int64_t latencia = esp_timer_get_time() - ev.marca_us;

        const char *nombre = "?";
        for (size_t i = 0; i < BOTONES_N; i++) {
            if (BOTONES[i].canal == canal) {
                nombre = BOTONES[i].nombre;
            }
        }

        xSemaphoreTake(s_mutex_estado, portMAX_DELAY);
        s_estado.toques++;
        if (canal == BOTONES[0].canal) {                 /* PLAY */
            s_estado.carga_activa = !s_estado.carga_activa;

            enviar_evento(EVENTO_CARGA,
                  s_estado.carga_activa ? 1 : 0);

        } else if (canal == BOTONES[1].canal) {          /* VOL_UP */
            if (s_estado.umbral_x100 == UMBRAL_INICIAL_X100) {
                s_estado.umbral_x100 = UMBRAL_ALTO_X100;
            } else if (s_estado.umbral_x100 == UMBRAL_ALTO_X100) {
                s_estado.umbral_x100 = UMBRAL_BAJO_X100;
            } else {
                s_estado.umbral_x100 = UMBRAL_INICIAL_X100;
            }
            enviar_evento(EVENTO_UMBRAL, s_estado.umbral_x100);
        }
        const modo_t modo = s_estado.modo;
        xSemaphoreGive(s_mutex_estado);

        if (canal == BOTONES[2].canal) {                 /* VOL_DOWN */
            /* Dispara el temporizador software ahora mismo, sin esperar a que
             * venza. xTimerReset() desde una tarea es seguro: la petición
             * viaja por la cola del servicio de temporizadores. */
            xTimerReset(s_timer_informe, pdMS_TO_TICKS(10));
        }

        ESP_LOGI(TAG, "[tactil] %s (T%d) -> modo %s  "
                      "| latencia ISR->tarea %" PRId64 " us",
                 nombre, canal, NOMBRE_MODO[modo], latencia);
    }
}

/* ======================================================================== */
/*  TAREA 4 — interfaz:  LED y panel por consola                             */
/* ======================================================================== */

/* Prioridad 3, la más baja de la aplicación: si se retrasa 50 ms, nadie se
 * entera. Despierta por notificación (dato nuevo) o por plazo, lo que llegue
 * antes: así el panel se refresca aunque el sensor se haya parado. */
static void tarea_interfaz(void *arg)
{
    char buf_ult[16], buf_med[16], buf_min[16], buf_max[16];
    bool led_alarma_encendido = false;
    
    /* Ya estamos listos: liberamos al sensor. */
    xSemaphoreGive(s_sem_arranque);

    while (1) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(INTERFAZ_PERIODO_MS));

        /* Copia local del estado bajo mutex; todo el trabajo lento —formatear
         * y escribir por la UART— se hace DESPUES de soltarlo. Retener un
         * mutex mientras se imprime es el error clásico. */
        xSemaphoreTake(s_mutex_estado, portMAX_DELAY);
        const estado_t e = s_estado;
        xSemaphoreGive(s_mutex_estado);

        if (e.muestras == 0) {
            continue;
        }

        formatear_temp(buf_ult, sizeof(buf_ult), e.ultima_x100);
        formatear_temp(buf_med, sizeof(buf_med), e.media_x100);
        formatear_temp(buf_min, sizeof(buf_min), e.minima_x100);
        formatear_temp(buf_max, sizeof(buf_max), e.maxima_x100);

        /* El LED como termómetro: azul frío, verde templado, rojo caliente.
         * En MODO_SILENCIO se apaga, para poder mirar el consumo. */
        if (e.alarma) {
        if (s_parpadeo_alarma) {
            led(24, 0, 0);
        } else {
            led(0, 0, 0);
        }
        } else if (e.modo == MODO_SILENCIO) {
            ESP_ERROR_CHECK(led_strip_clear(s_led));
        } else if (e.media_x100 < 3000) {
            led(0, 0, 24);
        } else if (e.media_x100 < 4000) {
            led(0, 24, 0);
        } else {
            led(24, 0, 0);
        }

        ESP_LOGI(TAG, "[interfaz] %s C  media %s  min %s  max %s  "
                      "| n=%" PRIu32 "  toques=%" PRIu32 "  modo=%s",
                 buf_ult, buf_med, buf_min, buf_max,
                 e.muestras, e.toques, NOMBRE_MODO[e.modo]);
    }
}

/* ======================================================================== */
/*  TAREA — carga:  atiende la carga             */
/* ======================================================================== */


static void tarea_carga(void *arg)
{
    volatile uint32_t basura = 0;

    while (1) {
        xSemaphoreTake(s_mutex_estado, portMAX_DELAY);
        const bool activa = s_estado.carga_activa;
        xSemaphoreGive(s_mutex_estado);

        if (activa) {
            ///* TAREA carga: atiende la carga */
            for (volatile uint32_t i = 0; i < 100000; i++) {
                basura = basura * 1664525u + 1013904223u;
            }

            taskYIELD();   // ← ESTA ES LA CLAVE
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}


/* ======================================================================== */
/*  TAREA — registro:  atiende los toques diferidos de la ISR                */
/* ======================================================================== */

static void tarea_registro(void *arg)
{
    evento_t evento;

    while (1) {
        if (xQueueReceive(s_cola_eventos, &evento, portMAX_DELAY) == pdTRUE) {

            switch (evento.tipo) {
            case EVENTO_TOQUE:
                ESP_LOGI(TAG, "[registro] TOQUE T%" PRId32,
                         evento.valor);
                break;

            case EVENTO_CARGA:
                ESP_LOGI(TAG, "[registro] CARGA %s",
                         evento.valor ? "ON" : "OFF");
                break;

            case EVENTO_UMBRAL:
                ESP_LOGI(TAG, "[registro] UMBRAL %" PRId32 " C",
                         evento.valor / 100);
                break;

            case EVENTO_ALARMA:
                ESP_LOGI(TAG, "[registro] ALARMA %s",
                         evento.valor ? "ON" : "OFF");
                break;
            }
        }
    }
}

/* ======================================================================== */
/*  TEMPORIZADOR SOFTWARE — informe de tareas                                */
/* ======================================================================== */

static const char *nombre_estado(eTaskState s)
{
    switch (s) {
    case eRunning:   return "EJECUTANDO";
    case eReady:     return "LISTA";
    case eBlocked:   return "BLOQUEADA";
    case eSuspended: return "SUSPENDIDA";
    case eDeleted:   return "BORRADA";
    default:         return "?";
    }
}

/* El callback de un temporizador software NO es una ISR: lo ejecuta la tarea
 * del servicio de temporizadores (`Tmr Svc`). Aun así hay una regla de oro:
 * NUNCA bloquearse aquí dentro, porque se retrasarían todos los demás
 * temporizadores del sistema. Por eso este callback solo lee y escribe. */
static void informe_de_tareas(TimerHandle_t t)
{
    static const char *TAREAS[] = { "tactil", "sensor", "proceso", "interfaz", "carga" };

    ESP_LOGI(TAG, "--- Informe de tareas (cada %d s) ------------------------",
             INFORME_PERIODO_MS / 1000);
    ESP_LOGI(TAG, "  %-10s %-4s %-11s %-8s", "tarea", "prio", "estado",
             "pila libre");

    for (size_t i = 0; i < sizeof(TAREAS) / sizeof(TAREAS[0]); i++) {
        const TaskHandle_t h = xTaskGetHandle(TAREAS[i]);
        if (h == NULL) {
            continue;
        }
        /* El "high water mark" es lo que le SOBRO a la pila, en bytes, no lo
         * que gastó. Si sale 200, estás a 200 bytes de desbordar. */
        ESP_LOGI(TAG, "  %-10s %-4u %-11s %" PRIu32 " B",
                 TAREAS[i],
                 (unsigned)uxTaskPriorityGet(h),
                 nombre_estado(eTaskGetState(h)),
                 (uint32_t)uxTaskGetStackHighWaterMark(h));
    }

    ESP_LOGI(TAG, "  cola de muestras: %u/%d llenas, %" PRIu32 " perdidas",
             (unsigned)uxQueueMessagesWaiting(s_cola_muestras),
             COLA_MUESTRAS_LARGO, s_perdidas_cola);
    ESP_LOGI(TAG, "  jitter maximo del sensor: %" PRIu32 " us  "
                  "(periodo %d ms, tick %d ms)",
             s_jitter_max_us, SENSOR_PERIODO_MS,
             (int)(1000 / configTICK_RATE_HZ));
    ESP_LOGI(TAG, "  heap libre: %" PRIu32 " B  (minimo historico %" PRIu32 " B)",
             (uint32_t)esp_get_free_heap_size(),
             (uint32_t)esp_get_minimum_free_heap_size());
}

/* ======================================================================== */
/*  Periféricos                                                              */
/* ======================================================================== */

static void configurar_led(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = RGB_LED_GPIO,
        .max_leds = RGB_LED_COUNT,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000, /* 10 MHz */
        .flags.with_dma = false,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_led));
    ESP_ERROR_CHECK(led_strip_clear(s_led));
}

/* Sensor interno del ESP32-S2. Mide la temperatura DEL CHIP, no la del aire:
 * sube varios grados en cuanto la CPU trabaja. Para esta clase da igual —lo
 * que importa es que es una fuente de datos real y periódica—, pero conviene
 * saberlo antes de sacar conclusiones meteorológicas. */
static void configurar_sensor(void)
{
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);

    ESP_ERROR_CHECK(temperature_sensor_install(&cfg, &s_tsens));
    ESP_ERROR_CHECK(temperature_sensor_enable(s_tsens));
}

static touch_channel_config_t config_canal_por_defecto(void)
{
    touch_channel_config_t cfg = {
        .active_thresh = { UMBRAL_INICIAL },
        .charge_speed = TOUCH_CHARGE_SPEED_7,
        .init_charge_volt = TOUCH_INIT_CHARGE_VOLT_DEFAULT,
    };
    return cfg;
}

/* Mide la línea base real de cada canal y fija el umbral como un porcentaje
 * de ella. Ver clase-2/detalles/touch-capacitivo-esp32s2.md. */
static void calibrar_umbrales(void)
{
    ESP_LOGI(TAG, "Calibrando... NO toques la placa durante el arranque");

    ESP_ERROR_CHECK(touch_sensor_enable(s_touch));
    for (int i = 0; i < BARRIDOS_CALIBRACION; i++) {
        ESP_ERROR_CHECK(touch_sensor_trigger_oneshot_scanning(s_touch, 2000));
    }
    ESP_ERROR_CHECK(touch_sensor_disable(s_touch));

    for (size_t i = 0; i < BOTONES_N; i++) {
        uint32_t benchmark[TOUCH_SAMPLE_CFG_NUM] = { 0 };
        ESP_ERROR_CHECK(touch_channel_read_data(s_canales[i],
                                                TOUCH_CHAN_DATA_TYPE_BENCHMARK,
                                                benchmark));

        touch_channel_config_t cfg = config_canal_por_defecto();
        cfg.active_thresh[0] = (uint32_t)(benchmark[0] * UMBRAL_RATIO);
        ESP_ERROR_CHECK(touch_sensor_reconfig_channel(s_canales[i], &cfg));

        ESP_LOGI(TAG, "  %-8s T%-2d  benchmark=%" PRIu32 "  umbral=%" PRIu32,
                 BOTONES[i].nombre, BOTONES[i].canal, benchmark[0],
                 cfg.active_thresh[0]);
    }
}

static void configurar_tactil(void)
{
    touch_sensor_sample_config_t muestreo[TOUCH_SAMPLE_CFG_NUM] = {
        TOUCH_SENSOR_V2_DEFAULT_SAMPLE_CONFIG(500, TOUCH_VOLT_LIM_L_0V5,
                                                   TOUCH_VOLT_LIM_H_2V2),
    };
    touch_sensor_config_t sens_cfg =
        TOUCH_SENSOR_DEFAULT_BASIC_CONFIG(TOUCH_SAMPLE_CFG_NUM, muestreo);
    ESP_ERROR_CHECK(touch_sensor_new_controller(&sens_cfg, &s_touch));

    touch_channel_config_t chan_cfg = config_canal_por_defecto();
    for (size_t i = 0; i < BOTONES_N; i++) {
        ESP_ERROR_CHECK(touch_sensor_new_channel(s_touch, BOTONES[i].canal,
                                                 &chan_cfg, &s_canales[i]));
    }

    touch_sensor_filter_config_t filtro = TOUCH_SENSOR_DEFAULT_FILTER_CONFIG();
    ESP_ERROR_CHECK(touch_sensor_config_filter(s_touch, &filtro));

    calibrar_umbrales();

    touch_event_callbacks_t callbacks = { .on_active = al_tocar };
    ESP_ERROR_CHECK(touch_sensor_register_callbacks(s_touch, &callbacks, NULL));

    ESP_ERROR_CHECK(touch_sensor_enable(s_touch));
    ESP_ERROR_CHECK(touch_sensor_start_continuous_scanning(s_touch));
}

/* ======================================================================== */
/*  main                                                                     */
/* ======================================================================== */

void app_main(void)
{
    ESP_LOGI(TAG, "=== multitarea: cuatro tareas de FreeRTOS en la Kaluga-1 ===");
    ESP_LOGI(TAG, "PLAY = cambia de modo | VOL_UP = reinicia min/max | "
                  "VOL_DOWN = informe ya");

    configurar_led();
    led(24, 12, 0);                     /* ambar: arrancando */
    configurar_sensor();

    /* --- Objetos del kernel, ANTES de las tareas que los usan ------------
     *
     * Todos devuelven NULL si no hay heap. Comprobarlo no es paranoia: en un
     * embebido es el modo de fallo más común, y falla en el arranque, que es
     * el mejor momento posible para enterarse. */
    s_cola_muestras = xQueueCreate(COLA_MUESTRAS_LARGO, sizeof(muestra_t));
    s_cola_toques   = xQueueCreate(COLA_TOQUES_LARGO, sizeof(toque_t));
    s_mutex_estado  = xSemaphoreCreateMutex();
    s_sem_arranque  = xSemaphoreCreateBinary();

    s_cola_eventos = xQueueCreate(COLA_EVENTOS_LARGO, sizeof(evento_t));

    s_timer_parpadeo = xTimerCreate("parpadeo", pdMS_TO_TICKS(250), pdTRUE, NULL, 
       callback_parpadeo
    );

    ESP_ERROR_CHECK(s_timer_parpadeo != NULL ? ESP_OK : ESP_ERR_NO_MEM);

    ESP_ERROR_CHECK(
        xTimerStart(s_timer_parpadeo, pdMS_TO_TICKS(100)) == pdPASS
        ? ESP_OK
        : ESP_FAIL
    );

    ESP_ERROR_CHECK((s_cola_muestras && s_cola_toques &&
                     s_mutex_estado && s_sem_arranque) ? ESP_OK : ESP_ERR_NO_MEM);

    /* La cola de toques debe existir antes de habilitar la interrupción. */
    configurar_tactil();

    ESP_ERROR_CHECK((s_cola_muestras && s_cola_toques &&
                 s_cola_eventos &&
                 s_mutex_estado && s_sem_arranque)
                ? ESP_OK : ESP_ERR_NO_MEM);


    /* --- Las tareas ------------------------------------------------------
     *
     * Nada más crearse, una tarea de prioridad mayor que app_main (que corre
     * a 1) DESALOJA a app_main y empieza a ejecutarse. Por eso el orden de
     * creación importa menos de lo que parece: lo que ordena el arranque es
     * el semáforo s_sem_arranque, no la secuencia de xTaskCreate(). */
    xTaskCreate(tarea_tactil,   "tactil",   PILA_TACTIL,   NULL, PRIO_TACTIL,   NULL);
    xTaskCreate(tarea_sensor,   "sensor",   PILA_SENSOR,   NULL, PRIO_SENSOR,   NULL);
    xTaskCreate(tarea_proceso,  "proceso",  PILA_PROCESO,  NULL, PRIO_PROCESO,  NULL);
    xTaskCreate(tarea_interfaz, "interfaz", PILA_INTERFAZ, NULL, PRIO_INTERFAZ,
                &s_tarea_interfaz);
    xTaskCreate(tarea_carga, "carga", PILA_CARGA, NULL,
            PRIO_CARGA, &s_tarea_carga);
    xTaskCreate(tarea_registro, "registro", PILA_REGISTRO, NULL, PRIO_REGISTRO,NULL);

    /* --- El temporizador software ----------------------------------------
     *
     * No consume una tarea propia ni una pila propia: lo atiende la tarea
     * `Tmr Svc` que ESP-IDF ya tiene arrancada. Para trabajo periódico y
     * ligero es mucho más barato que crear una tarea con un vTaskDelay(). */
    s_timer_informe = xTimerCreate("informe", pdMS_TO_TICKS(INFORME_PERIODO_MS),
                                   pdTRUE, NULL, informe_de_tareas);
    ESP_ERROR_CHECK(s_timer_informe != NULL ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTimerStart(s_timer_informe, pdMS_TO_TICKS(100)) == pdPASS
                        ? ESP_OK : ESP_FAIL);

    ESP_LOGI(TAG, "Arranque completo. app_main termina; el planificador sigue.");

    /* Y aquí acaba app_main. ESP-IDF borra su tarea y libera su pila; las
     * cuatro tareas y el temporizador siguen corriendo. En FreeRTOS no hay
     * "programa principal": hay tareas. */
}
