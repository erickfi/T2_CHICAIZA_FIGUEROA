
#include "periodica.h"

#include <limits.h>
#include <stdint.h>
#include <inttypes.h>

#include "bme280.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define NUM_ACTIVACIONES 50U
#define CANTIDAD_MODOS 4U
#define PRIORIDAD_SENSOR 3U

static const char *TAG = "PERIODICA";

static TaskHandle_t s_tarea = NULL;
static portMUX_TYPE s_mux =
    portMUX_INITIALIZER_UNLOCKED;

static periodica_resumen_t
    s_ultimos[CANTIDAD_MODOS];

typedef struct {
    uint32_t activaciones;
    uint32_t intervalos;
    uint32_t rebases;
    uint32_t errores_sensor;

    int64_t anterior_us;
    int64_t suma_us;
    int64_t minimo_us;
    int64_t maximo_us;
} ventana_t;

/* Comienza una ventana estadistica limpia. */
static void reiniciar_ventana(ventana_t *v)
{
    *v = (ventana_t){
        .minimo_us = INT64_MAX
    };
}

/* Publica un resumen sin mezclar modos de carga. */
static void publicar_resumen(
    const ventana_t *v,
    carga_modo_t modo
)
{
    if (v->intervalos == 0U) {
        return;
    }

    periodica_resumen_t r = {
        .valido = true,
        .modo = modo,
        .activaciones = v->activaciones,
        .intervalos = v->intervalos,
        .rebases = v->rebases,
        .errores_sensor = v->errores_sensor,

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

    portENTER_CRITICAL(&s_mux);

    s_ultimos[(unsigned)modo] = r;

    portEXIT_CRITICAL(&s_mux);

    ESP_LOGI(
        TAG,
        "modo=%u n=%" PRIu32
        " intervalos=%" PRIu32
        " medio=%.3f min=%.3f max=%.3f"
        " jitter=%.3f ms rebases=%" PRIu32
        " errores_sensor=%" PRIu32,
        (unsigned)modo,
        r.activaciones,
        r.intervalos,
        r.medio_ms,
        r.minimo_ms,
        r.maximo_ms,
        r.jitter_ms,
        r.rebases,
        r.errores_sensor
    );
}

static void tarea_periodica(void *argumento)
{
    uint32_t periodo_ms =
        (uint32_t)(uintptr_t)argumento;

    const TickType_t ticks =
        pdMS_TO_TICKS(periodo_ms);

    TickType_t referencia =
        xTaskGetTickCount();

    ventana_t v;
    reiniciar_ventana(&v);

    uint32_t version_anterior = UINT32_MAX;
    carga_modo_t modo_ventana = CARGA_PARADA;

    for (;;) {

        /* Espera absoluta. */
        BaseType_t a_tiempo =
            xTaskDelayUntil(&referencia, ticks);

        /* Marca temporal ANTES de leer I2C. */
        const int64_t ahora_us =
            esp_timer_get_time();

        carga_estado_t estado_carga;

        if (carga_obtener_estado(
                &estado_carga
            ) != ESP_OK) {
            continue;
        }

        /* Descartar datos al cambiar la carga. */
        if (version_anterior !=
            estado_carga.generacion) {

            reiniciar_ventana(&v);

            version_anterior =
                estado_carga.generacion;

            modo_ventana =
                estado_carga.modo;
        }

        if (v.anterior_us != 0) {

            const int64_t intervalo =
                ahora_us - v.anterior_us;

            v.suma_us += intervalo;

            if (intervalo < v.minimo_us) {
                v.minimo_us = intervalo;
            }

            if (intervalo > v.maximo_us) {
                v.maximo_us = intervalo;
            }

            ++v.intervalos;
        }

        if (v.activaciones != 0 &&
            a_tiempo == pdFALSE) {
            ++v.rebases;
        }

        v.anterior_us = ahora_us;
        ++v.activaciones;

        /* Esta tarea es la propietaria de la lectura. */
        if (bme280_leer() != ESP_OK) {
            ++v.errores_sensor;
        }

        /*
         * Si el modo cambio durante I2C,
         * descartar tambien esta ventana.
         */
        carga_estado_t despues;

        if (carga_obtener_estado(
                &despues
            ) == ESP_OK &&
            despues.generacion != version_anterior) {

            reiniciar_ventana(&v);
            version_anterior = despues.generacion;
            modo_ventana = despues.modo;

            continue;
        }

        if (v.activaciones >= NUM_ACTIVACIONES) {

            publicar_resumen(
                &v,
                modo_ventana
            );

            reiniciar_ventana(&v);
        }
    }
}

esp_err_t periodica_iniciar(uint32_t periodo_ms)
{
    if (periodo_ms < 100U ||
        pdMS_TO_TICKS(periodo_ms) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_tarea != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xTaskCreatePinnedToCore(
            tarea_periodica,
            "periodica",
            4096,
            (void *)(uintptr_t)periodo_ms,
            PRIORIDAD_SENSOR,
            &s_tarea,
            0
        ) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(
        TAG,
        "periodo=%" PRIu32
        " ms, CPU0, prioridad=%u",
        periodo_ms,
        (unsigned)PRIORIDAD_SENSOR
    );

    return ESP_OK;
}

esp_err_t periodica_ultimo_resumen(
    carga_modo_t modo,
    periodica_resumen_t *resumen
)
{
    if (resumen == NULL ||
        (int)modo < 0 ||
        (unsigned)modo >= CANTIDAD_MODOS) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_mux);

    *resumen = s_ultimos[(unsigned)modo];

    portEXIT_CRITICAL(&s_mux);

    return resumen->valido ?
        ESP_OK : ESP_ERR_NOT_FOUND;
}
