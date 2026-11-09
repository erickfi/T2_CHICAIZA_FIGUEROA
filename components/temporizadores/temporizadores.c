
#include "temporizadores.h"

#include <inttypes.h>
#include <limits.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#define P4_FUENTES                2U
#define P4_MODOS                  4U
#define P4_MUESTRAS               50U
#define P4_COLA_LONGITUD          64U
#define P4_PILA_MEDIDA_BYTES      4096U
#define P4_PRIORIDAD_MEDIDA       4U

/* Limites de operacion elegidos para esta practica. */
#define P4_MIN_PERIODO_MS         5U
#define P4_MAX_PERIODO_MS         1000U

static const char *TAG = "P4_TIMERS";

/*
 * Epoca par: medicion habilitada.
 * Epoca impar: temporizadores reconfigurandose.
 *
 * Los callbacks usan accesos atomicos para evitar
 * esperar por un mutex de control.
 */
static uint32_t s_epoca = 2U;
static uint32_t s_periodo_ms = 0U;
static uint32_t s_secuencia[P4_FUENTES];
static uint32_t s_perdidos[P4_FUENTES];
static bool s_iniciado = false;

static QueueHandle_t s_cola = NULL;
static SemaphoreHandle_t s_mutex_control = NULL;
static TaskHandle_t s_tarea_medida = NULL;

static esp_timer_handle_t s_alarma = NULL;
static TimerHandle_t s_timer_sw = NULL;

static portMUX_TYPE s_mux_resumen =
    portMUX_INITIALIZER_UNLOCKED;

/* Evento enviado desde cada callback. */
typedef struct {
    int64_t instante_us;
    uint32_t epoca;
    uint32_t secuencia;
    uint32_t periodo_ms;
    uint32_t generacion_carga;
    carga_modo_t modo;
    temporizador_origen_t origen;
} p4_evento_t;

/* Acumulador privado de la tarea de medida. */
typedef struct {
    bool tiene_secuencia;

    uint32_t epoca;
    uint32_t secuencia;
    uint32_t periodo_ms;
    uint32_t generacion_carga;
    carga_modo_t modo;

    uint32_t activaciones;
    uint32_t intervalos;
    uint32_t rebases;

    int64_t instante_anterior_us;
    int64_t suma_us;
    int64_t minimo_us;
    int64_t maximo_us;
} p4_ventana_t;

/* Un resumen por origen y modo de carga. */
typedef struct {
    temporizadores_resumen_t datos;
    uint32_t epoca;
} p4_resumen_guardado_t;

static p4_resumen_guardado_t
    s_ultimos[P4_FUENTES][P4_MODOS];

static bool origen_valido(temporizador_origen_t origen)
{
    return (
        origen == TEMPORIZADOR_ESP ||
        origen == TEMPORIZADOR_FREERTOS
    );
}

/*
 * Captura el instante en el contexto del callback.
 *
 * No imprime, no espera y no hace calculos
 * estadisticos.
 */
static void registrar_evento(temporizador_origen_t origen)
{
    const int64_t instante_us = esp_timer_get_time();

    const uint32_t epoca = __atomic_load_n(
        &s_epoca,
        __ATOMIC_ACQUIRE
    );

    if ((epoca & 1U) != 0U) {
        return;
    }

    const uint32_t periodo = __atomic_load_n(
        &s_periodo_ms,
        __ATOMIC_ACQUIRE
    );

    /*
     * xTimerChangePeriod() encola la modificacion.
     * Descartamos callbacks SW hasta que su nuevo
     * periodo se haya aplicado realmente.
     */
    if (origen == TEMPORIZADOR_FREERTOS &&
        xTimerGetPeriod(s_timer_sw) !=
            pdMS_TO_TICKS(periodo)) {
        return;
    }

    /*
     * Consulta breve de contexto de P1.
     * Se toma DESPUES de la marca temporal.
     */
    carga_estado_t carga = {0};

    if (carga_obtener_estado(&carga) != ESP_OK) {
        return;
    }

    if (epoca != __atomic_load_n(
            &s_epoca,
            __ATOMIC_ACQUIRE)) {
        return;
    }

    p4_evento_t evento = {
        .instante_us = instante_us,
        .epoca = epoca,

        .secuencia = __atomic_add_fetch(
            &s_secuencia[origen],
            1U,
            __ATOMIC_RELAXED
        ),

        .periodo_ms = periodo,
        .generacion_carga = carga.generacion,
        .modo = carga.modo,
        .origen = origen
    };

    /*
     * Tiempo de espera cero.
     * Ante cola llena, contar la perdida.
     */
    if (xQueueSend(
            s_cola,
            &evento,
            0
        ) != pdTRUE) {

        __atomic_add_fetch(
            &s_perdidos[origen],
            1U,
            __ATOMIC_RELAXED
        );
    }
}

/* Callback ejecutado por la tarea esp_timer. */
static void respuesta_esp_timer(void *arg)
{
    (void)arg;
    registrar_evento(TEMPORIZADOR_ESP);
}

/* Callback ejecutado por la tarea de servicio FreeRTOS. */
static void respuesta_sw_timer(TimerHandle_t timer)
{
    (void)timer;
    registrar_evento(TEMPORIZADOR_FREERTOS);
}

/* Prepara una ventana estadistica nueva. */
static void reiniciar_ventana(
    p4_ventana_t *ventana,
    const p4_evento_t *evento
)
{
    memset(ventana, 0, sizeof(*ventana));

    ventana->epoca = evento->epoca;
    ventana->generacion_carga = evento->generacion_carga;
    ventana->modo = evento->modo;
    ventana->periodo_ms = evento->periodo_ms;
    ventana->minimo_us = INT64_MAX;
}

/* Guarda y publica una ventana completa. */
static void publicar_resumen(
    temporizador_origen_t origen,
    const p4_ventana_t *v
)
{
    if (v->intervalos == 0U) {
        return;
    }

    temporizadores_resumen_t r = {
        .valido = true,
        .modo = v->modo,
        .periodo_ms = v->periodo_ms,
        .activaciones = v->activaciones,
        .intervalos = v->intervalos,
        .rebases = v->rebases,

        .perdidos_total = __atomic_load_n(
            &s_perdidos[origen],
            __ATOMIC_RELAXED
        ),

        .medio_ms =
            (double)v->suma_us /
            (double)v->intervalos / 1000.0,

        .minimo_ms =
            (double)v->minimo_us / 1000.0,

        .maximo_ms =
            (double)v->maximo_us / 1000.0,

        .jitter_ms =
            (double)(v->maximo_us -
                     v->minimo_us) / 1000.0
    };

    portENTER_CRITICAL(&s_mux_resumen);

    s_ultimos[origen][v->modo].datos = r;
    s_ultimos[origen][v->modo].epoca = v->epoca;

    portEXIT_CRITICAL(&s_mux_resumen);

    ESP_LOGI(
        TAG,
        "%s modo=%u T=%" PRIu32
        "ms n=%" PRIu32
        " medio=%.3f min=%.3f max=%.3f"
        " jitter=%.3f ms"
        " rebases=%" PRIu32
        " perdidos_total=%" PRIu32,

        origen == TEMPORIZADOR_ESP ?
            "esp_timer" : "FreeRTOS",

        (unsigned)v->modo,
        r.periodo_ms,
        r.activaciones,
        r.medio_ms,
        r.minimo_ms,
        r.maximo_ms,
        r.jitter_ms,
        r.rebases,
        r.perdidos_total
    );
}

/*
 * Tarea de medida, fijada al nucleo 1.
 *
 * Recibe los instantes de ambas fuentes y
 * calcula estadisticas independientes.
 */
static void tarea_medida(void *arg)
{
    (void)arg;

    p4_ventana_t ventanas[P4_FUENTES] = {0};
    p4_evento_t evento;

    for (;;) {

        if (xQueueReceive(
                s_cola,
                &evento,
                portMAX_DELAY
            ) != pdTRUE) {
            continue;
        }

        if (!origen_valido(evento.origen) ||
            (unsigned)evento.modo >= P4_MODOS) {
            continue;
        }

        /* Descartar eventos de periodos anteriores. */
        if (evento.epoca != __atomic_load_n(
                &s_epoca,
                __ATOMIC_ACQUIRE)) {
            continue;
        }

        p4_ventana_t *v = &ventanas[evento.origen];

        /*
         * Reiniciar ante:
         * - cambio de periodo,
         * - cambio de modo de carga,
         * - perdida de un evento,
         * - primera activacion.
         */
        if (!v->tiene_secuencia ||
            v->epoca != evento.epoca ||
            v->generacion_carga !=
                evento.generacion_carga ||
            v->modo != evento.modo ||
            v->periodo_ms != evento.periodo_ms ||
            evento.secuencia != v->secuencia + 1U) {

            reiniciar_ventana(v, &evento);
        }

        if (v->activaciones != 0U) {

            const int64_t intervalo_us =
                evento.instante_us -
                v->instante_anterior_us;

            if (intervalo_us <= 0) {

                reiniciar_ventana(v, &evento);

            } else {

                v->suma_us += intervalo_us;

                if (intervalo_us < v->minimo_us) {
                    v->minimo_us = intervalo_us;
                }

                if (intervalo_us > v->maximo_us) {
                    v->maximo_us = intervalo_us;
                }

                ++v->intervalos;

                /*
                 * Indicador operativo de rebase:
                 * intervalo observado >= 2 periodos.
                 *
                 * No es el retorno de xTaskDelayUntil;
                 * son mecanismos temporales diferentes.
                 */
                if (intervalo_us >=
                    2LL * (int64_t)v->periodo_ms *
                        1000LL) {

                    ++v->rebases;
                }
            }
        }

        v->instante_anterior_us = evento.instante_us;
        v->secuencia = evento.secuencia;
        v->tiene_secuencia = true;

        ++v->activaciones;

        if (v->activaciones == P4_MUESTRAS) {

            publicar_resumen(evento.origen, v);

            /* Iniciar otra ventana del mismo modo. */
            v->activaciones = 0U;
            v->intervalos = 0U;
            v->rebases = 0U;
            v->instante_anterior_us = 0;
            v->suma_us = 0;
            v->minimo_us = INT64_MAX;
            v->maximo_us = 0;
        }
    }
}

/*
 * Liberacion si falla alguna fase de inicio.
 * No se reutiliza este camino en funcionamiento.
 */
static void limpiar_inicio(void)
{
    if (s_alarma != NULL) {

        if (esp_timer_is_active(s_alarma)) {
            (void)esp_timer_stop(s_alarma);
        }

        (void)esp_timer_delete(s_alarma);
        s_alarma = NULL;
    }

    if (s_timer_sw != NULL) {

        if (xTimerDelete(s_timer_sw, 0) != pdPASS) {
            ESP_LOGE(
                TAG,
                "No se pudo encolar eliminacion timer SW"
            );
        }

        s_timer_sw = NULL;
    }

    if (s_tarea_medida != NULL) {
        vTaskDelete(s_tarea_medida);
        s_tarea_medida = NULL;
    }

    if (s_cola != NULL) {
        vQueueDelete(s_cola);
        s_cola = NULL;
    }

    if (s_mutex_control != NULL) {
        vSemaphoreDelete(s_mutex_control);
        s_mutex_control = NULL;
    }
}

esp_err_t temporizadores_iniciar(uint32_t periodo_ms)
{
    if (s_iniciado) {
        return ESP_ERR_INVALID_STATE;
    }

    if (periodo_ms < P4_MIN_PERIODO_MS ||
        periodo_ms > P4_MAX_PERIODO_MS ||
        pdMS_TO_TICKS(periodo_ms) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_OK;

    s_mutex_control = xSemaphoreCreateMutex();

    if (s_mutex_control == NULL) {
        err = ESP_ERR_NO_MEM;
        goto fallo;
    }

    s_cola = xQueueCreate(
        P4_COLA_LONGITUD,
        sizeof(p4_evento_t)
    );

    if (s_cola == NULL) {
        err = ESP_ERR_NO_MEM;
        goto fallo;
    }

    /* Alarma de esp_timer; callback en tarea. */
    const esp_timer_create_args_t args = {
        .callback = respuesta_esp_timer,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "p4_esp"
    };

    err = esp_timer_create(&args, &s_alarma);

    if (err != ESP_OK) {
        goto fallo;
    }

    /* Temporizador software periodico de FreeRTOS. */
    s_timer_sw = xTimerCreate(
        "p4_sw",
        pdMS_TO_TICKS(periodo_ms),
        pdTRUE,
        NULL,
        respuesta_sw_timer
    );

    if (s_timer_sw == NULL) {
        err = ESP_ERR_NO_MEM;
        goto fallo;
    }

    __atomic_store_n(
        &s_periodo_ms,
        periodo_ms,
        __ATOMIC_RELEASE
    );

    /* Tarea de medida en CPU1, fuera de los callbacks. */
    if (xTaskCreatePinnedToCore(
            tarea_medida,
            "p4_medida",
            P4_PILA_MEDIDA_BYTES,
            NULL,
            P4_PRIORIDAD_MEDIDA,
            &s_tarea_medida,
            1
        ) != pdPASS) {

        err = ESP_ERR_NO_MEM;
        goto fallo;
    }

    /* Periodo de esp_timer expresado en us. */
    err = esp_timer_start_periodic(
        s_alarma,
        (uint64_t)periodo_ms * 1000ULL
    );

    if (err != ESP_OK) {
        goto fallo;
    }

    /* Temporizador FreeRTOS expresado en ticks. */
    if (xTimerStart(s_timer_sw, 0) != pdPASS) {
        err = ESP_FAIL;
        goto fallo;
    }

    s_iniciado = true;

    ESP_LOGI(
        TAG,
        "P4 iniciado, periodo=%" PRIu32
        "ms, medida CPU1 prioridad=%u",
        periodo_ms,
        (unsigned)P4_PRIORIDAD_MEDIDA
    );

    return ESP_OK;

fallo:
    limpiar_inicio();
    return err;
}

/*
 * Cambia los dos periodos de manera coordinada.
 *
 * Los callbacks descartan muestras mientras se
 * solicita el cambio.
 */
esp_err_t temporizadores_fijar_periodo(
    uint32_t periodo_ms
)
{
    if (!s_iniciado) {
        return ESP_ERR_INVALID_STATE;
    }

    if (periodo_ms < P4_MIN_PERIODO_MS ||
        periodo_ms > P4_MAX_PERIODO_MS ||
        pdMS_TO_TICKS(periodo_ms) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(
            s_mutex_control,
            portMAX_DELAY
        ) != pdTRUE) {
        return ESP_FAIL;
    }

    const uint32_t anterior = __atomic_load_n(
        &s_periodo_ms,
        __ATOMIC_ACQUIRE
    );

    if (periodo_ms == anterior) {
        xSemaphoreGive(s_mutex_control);
        return ESP_OK;
    }

    /* Epoca impar: suspender adquisicion de datos. */
    __atomic_add_fetch(
        &s_epoca,
        1U,
        __ATOMIC_ACQ_REL
    );

    esp_err_t err = esp_timer_restart(
        s_alarma,
        (uint64_t)periodo_ms * 1000ULL
    );

    if (err == ESP_OK) {

        if (xTimerChangePeriod(
                s_timer_sw,
                pdMS_TO_TICKS(periodo_ms),
                0
            ) != pdPASS) {

            err = ESP_FAIL;

            /* Intentar restaurar el periodo anterior. */
            esp_err_t recuperacion = esp_timer_restart(
                s_alarma,
                (uint64_t)anterior * 1000ULL
            );

            if (recuperacion != ESP_OK) {
                ESP_LOGE(
                    TAG,
                    "No se pudo restaurar esp_timer: %s",
                    esp_err_to_name(recuperacion)
                );
            }
        }
    }

    if (err == ESP_OK) {
        __atomic_store_n(
            &s_periodo_ms,
            periodo_ms,
            __ATOMIC_RELEASE
        );
    }

    /* Nueva epoca par: medicion habilitada. */
    __atomic_add_fetch(
        &s_epoca,
        1U,
        __ATOMIC_ACQ_REL
    );

    xSemaphoreGive(s_mutex_control);

    return err;
}

esp_err_t temporizadores_obtener_periodo(
    uint32_t *periodo_ms
)
{
    if (periodo_ms == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_iniciado) {
        return ESP_ERR_INVALID_STATE;
    }

    *periodo_ms = __atomic_load_n(
        &s_periodo_ms,
        __ATOMIC_ACQUIRE
    );

    return ESP_OK;
}

esp_err_t temporizadores_ultimo_resumen(
    temporizador_origen_t origen,
    carga_modo_t modo,
    temporizadores_resumen_t *resumen
)
{
    if (!origen_valido(origen) ||
        (unsigned)modo >= P4_MODOS ||
        resumen == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_iniciado) {
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&s_mux_resumen);

    p4_resumen_guardado_t guardado =
        s_ultimos[origen][modo];

    portEXIT_CRITICAL(&s_mux_resumen);

    const uint32_t epoca_actual = __atomic_load_n(
        &s_epoca,
        __ATOMIC_ACQUIRE
    );

    if ((epoca_actual & 1U) != 0U ||
        !guardado.datos.valido ||
        guardado.epoca != epoca_actual) {

        return ESP_ERR_NOT_FOUND;
    }

    *resumen = guardado.datos;
    return ESP_OK;
}

esp_err_t temporizadores_perdidos(
    temporizador_origen_t origen,
    uint32_t *total
)
{
    if (!origen_valido(origen) || total == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_iniciado) {
        return ESP_ERR_INVALID_STATE;
    }

    *total = __atomic_load_n(
        &s_perdidos[origen],
        __ATOMIC_RELAXED
    );

    return ESP_OK;
}
