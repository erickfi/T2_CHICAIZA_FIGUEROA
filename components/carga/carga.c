
#include "carga.h"

#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define CARGA_RAFAGA_US  20000LL
#define CARGA_ESPERA_MS  27U
#define CARGA_PAUSA_MS   47U

#define CARGA_PRIO_BASE  2U
#define CARGA_PRIO_ALTA  3U

static const char *TAG = "CARGA";

static portMUX_TYPE s_mux =
    portMUX_INITIALIZER_UNLOCKED;

static carga_modo_t s_modo = CARGA_PARADA;
static uint32_t s_generacion = 0;
static uint32_t s_contador_1 = 0;
static uint32_t s_contador_2 = 0;

static TaskHandle_t s_t1 = NULL;
static TaskHandle_t s_t2 = NULL;
static TaskHandle_t s_informe = NULL;
static bool s_iniciado = false;

static carga_modo_t leer_modo(void)
{
    return __atomic_load_n(
        &s_modo, __ATOMIC_ACQUIRE
    );
}

/* Ráfaga de cálculo de 20 ms. */
static void rafaga(uint32_t *contador)
{
    const int64_t comienzo = esp_timer_get_time();

    while (esp_timer_get_time() - comienzo <
           CARGA_RAFAGA_US) {

        __atomic_fetch_add(
            contador, 1U, __ATOMIC_RELAXED
        );
    }
}

/* Tarea de carga 1, núcleo 0. */
static void tarea_carga_1(void *arg)
{
    (void)arg;

    UBaseType_t prioridad = CARGA_PRIO_BASE;

    for (;;) {
        carga_modo_t modo = leer_modo();

        const UBaseType_t nueva_prioridad =
            (modo == CARGA_INANICION ||
             modo == CARGA_CORREGIDA) ?
            CARGA_PRIO_ALTA : CARGA_PRIO_BASE;

        if (prioridad != nueva_prioridad) {
            vTaskPrioritySet(NULL, nueva_prioridad);
            prioridad = nueva_prioridad;
        }

        if (modo == CARGA_PARADA) {

            vTaskDelay(
                pdMS_TO_TICKS(CARGA_PAUSA_MS)
            );

        } else if (modo == CARGA_INANICION) {

            /* Sin bloqueo: provoca inanición en CPU0. */
            while (leer_modo() == CARGA_INANICION) {
                __atomic_fetch_add(
                    &s_contador_1, 1U,
                    __ATOMIC_RELAXED
                );
            }

        } else {

            /* TURNOS y CORREGIDA sí bloquean. */
            rafaga(&s_contador_1);

            vTaskDelay(
                pdMS_TO_TICKS(CARGA_ESPERA_MS)
            );
        }
    }
}

/* Tarea de carga 2, núcleo 0. */
static void tarea_carga_2(void *arg)
{
    (void)arg;

    for (;;) {
        if (leer_modo() == CARGA_PARADA) {

            vTaskDelay(
                pdMS_TO_TICKS(CARGA_PAUSA_MS)
            );

        } else {

            rafaga(&s_contador_2);

            vTaskDelay(
                pdMS_TO_TICKS(CARGA_ESPERA_MS)
            );
        }
    }
}

/* Informe cada segundo en el núcleo 1. */
static void tarea_informe(void *arg)
{
    (void)arg;

    TickType_t referencia = xTaskGetTickCount();

    uint32_t anterior_1 = 0;
    uint32_t anterior_2 = 0;

    for (;;) {
        xTaskDelayUntil(
            &referencia,
            pdMS_TO_TICKS(1000)
        );

        carga_estado_t e;

        if (carga_obtener_estado(&e) == ESP_OK) {
            ESP_LOGI(
                TAG,
                "modo=%u cont1=%" PRIu32
                " (+%" PRIu32 ") cont2=%" PRIu32
                " (+%" PRIu32 ")",
                (unsigned)e.modo,
                e.contador_1,
                e.contador_1 - anterior_1,
                e.contador_2,
                e.contador_2 - anterior_2
            );

            anterior_1 = e.contador_1;
            anterior_2 = e.contador_2;
        }
    }
}

esp_err_t carga_iniciar(void)
{
    if (s_iniciado) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xTaskCreatePinnedToCore(
            tarea_carga_1, "carga1", 3072,
            NULL, CARGA_PRIO_BASE, &s_t1, 0
        ) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreatePinnedToCore(
            tarea_carga_2, "carga2", 3072,
            NULL, CARGA_PRIO_BASE, &s_t2, 0
        ) != pdPASS) {
        vTaskDelete(s_t1);
        s_t1 = NULL;
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreatePinnedToCore(
            tarea_informe, "informe", 3072,
            NULL, CARGA_PRIO_BASE, &s_informe, 1
        ) != pdPASS) {

        vTaskDelete(s_t1);
        vTaskDelete(s_t2);
        s_t1 = NULL;
        s_t2 = NULL;

        return ESP_ERR_NO_MEM;
    }

    s_iniciado = true;

    ESP_LOGI(
        TAG,
        "Carga CPU0; informe CPU1; modo inicial parada"
    );

    return ESP_OK;
}

esp_err_t carga_fijar_modo(carga_modo_t nuevo)
{
    if ((int)nuevo < (int)CARGA_PARADA ||
        nuevo > CARGA_CORREGIDA) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_iniciado) {
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&s_mux);

    if (__atomic_load_n(
            &s_modo, __ATOMIC_RELAXED
        ) != nuevo) {

        __atomic_store_n(
            &s_modo, nuevo, __ATOMIC_RELEASE
        );

        ++s_generacion;
    }

    portEXIT_CRITICAL(&s_mux);

    ESP_LOGI(TAG, "Modo solicitado: %u",
             (unsigned)nuevo);

    return ESP_OK;
}

esp_err_t carga_obtener_estado(
    carga_estado_t *estado
)
{
    if (estado == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_iniciado) {
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&s_mux);

    estado->modo = __atomic_load_n(
        &s_modo, __ATOMIC_RELAXED
    );

    estado->generacion = s_generacion;

    estado->contador_1 = __atomic_load_n(
        &s_contador_1, __ATOMIC_RELAXED
    );

    estado->contador_2 = __atomic_load_n(
        &s_contador_2, __ATOMIC_RELAXED
    );

    portEXIT_CRITICAL(&s_mux);

    return ESP_OK;
}
