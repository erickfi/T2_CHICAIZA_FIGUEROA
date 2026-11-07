
#include "pulsador.h"

#include <stdbool.h>
#include <stddef.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "PULSADOR";

#define LONGITUD_COLA 20U

static QueueHandle_t s_cola = NULL;
static TaskHandle_t s_tarea = NULL;

static gpio_num_t s_gpio;
static int s_nivel_activo;
static uint32_t s_antirrebote_ms;
static bool s_iniciado = false;

/* Estado protegido entre ISR y tarea. */
static portMUX_TYPE s_mux =
    portMUX_INITIALIZER_UNLOCKED;

static uint32_t s_pulsaciones = 0;
static uint32_t s_flancos_perdidos = 0;

/* P3: funcion de aviso a la aplicacion. */
static pulsador_callback_t s_callback = NULL;
static void *s_contexto_callback = NULL;

/*
 * ISR original:
 * Captura el instante y lo envia a una cola.
 * No se bloquea ni procesa el rebote.
 */
static void pulsador_isr(void *argumento)
{
    (void)argumento;

    int64_t instante_us = esp_timer_get_time();
    BaseType_t despertar = pdFALSE;

    if (xQueueSendFromISR(
            s_cola,
            &instante_us,
            &despertar
        ) != pdTRUE) {

        portENTER_CRITICAL_ISR(&s_mux);

        ++s_flancos_perdidos;

        portEXIT_CRITICAL_ISR(&s_mux);
    }

    if (despertar == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

/*
 * Tarea del pulsador:
 * Confirma el nivel estable despues del antirrebote.
 * Solo una pulsacion confirmada produce un aviso.
 */
static void tarea_pulsador(void *argumento)
{
    (void)argumento;

    int64_t instante_us;
    int64_t ultimo_cambio_us = -1;

    bool estaba_pulsado =
        (gpio_get_level(s_gpio) == s_nivel_activo);

    const int64_t guarda_us =
        (int64_t)s_antirrebote_ms * 1000LL;

    while (true) {

        if (xQueueReceive(
                s_cola,
                &instante_us,
                portMAX_DELAY
            ) != pdTRUE) {
            continue;
        }

        ESP_LOGD(
            TAG,
            "Flanco en %lld us",
            (long long)instante_us
        );

        if (ultimo_cambio_us >= 0 &&
            (instante_us - ultimo_cambio_us) <
                guarda_us) {
            continue;
        }

        /* Confirmacion fuera de la ISR. */
        vTaskDelay(
            pdMS_TO_TICKS(s_antirrebote_ms)
        );

        bool esta_pulsado =
            (gpio_get_level(s_gpio) == s_nivel_activo);

        if (esta_pulsado == estaba_pulsado) {
            continue;
        }

        estaba_pulsado = esta_pulsado;
        ultimo_cambio_us = instante_us;

        if (esta_pulsado) {

            uint32_t numero;

            portENTER_CRITICAL(&s_mux);

            numero = ++s_pulsaciones;

            portEXIT_CRITICAL(&s_mux);

            ESP_LOGI(
                TAG,
                "Pulsacion valida: %" PRIu32,
                numero
            );

            /*
             * P3:
             * Avisamos a la aplicacion desde esta tarea.
             * El callback no realiza llamadas LEDC:
             * unicamente encola una orden.
             */
            if (s_callback != NULL) {
                s_callback(s_contexto_callback);
            }
        }
    }
}

/*
 * P3:
 * Registro previo a la creacion de la tarea.
 */
esp_err_t pulsador_registrar_callback(
    pulsador_callback_t callback,
    void *contexto
)
{
    if (callback == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_iniciado || s_tarea != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_callback = callback;
    s_contexto_callback = contexto;

    return ESP_OK;
}

esp_err_t pulsador_iniciar(
    const pulsador_config_t *config
)
{
    if (config == NULL ||
        s_cola != NULL ||
        (int)config->gpio < 0 ||
        (int)config->gpio >= 64 ||
        (config->nivel_activo != 0 &&
         config->nivel_activo != 1) ||
        config->antirrebote_ms == 0U ||
        pdMS_TO_TICKS(config->antirrebote_ms) == 0) {

        return ESP_ERR_INVALID_ARG;
    }

    s_gpio = config->gpio;
    s_nivel_activo = config->nivel_activo;
    s_antirrebote_ms = config->antirrebote_ms;

    s_cola = xQueueCreate(
        LONGITUD_COLA,
        sizeof(int64_t)
    );

    if (s_cola == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* Polarizacion externa, como en Taller 1. */
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << s_gpio),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };

    esp_err_t error = gpio_config(&cfg);

    if (error != ESP_OK) {
        goto error_cola;
    }

    if (xTaskCreate(
            tarea_pulsador,
            "tarea_pulsador",
            3072,
            NULL,
            10,
            &s_tarea
        ) != pdPASS) {

        error = ESP_ERR_NO_MEM;
        goto error_cola;
    }

    error = gpio_install_isr_service(0);

    if (error != ESP_OK &&
        error != ESP_ERR_INVALID_STATE) {
        goto error_tarea;
    }

    error = gpio_isr_handler_add(
        s_gpio,
        pulsador_isr,
        NULL
    );

    if (error != ESP_OK) {
        goto error_tarea;
    }

    error = gpio_set_intr_type(
        s_gpio,
        GPIO_INTR_ANYEDGE
    );

    if (error != ESP_OK) {
        gpio_isr_handler_remove(s_gpio);
        goto error_tarea;
    }

    error = gpio_intr_enable(s_gpio);

    if (error != ESP_OK) {
        gpio_isr_handler_remove(s_gpio);
        goto error_tarea;
    }

    s_iniciado = true;

    ESP_LOGI(
        TAG,
        "GPIO=%d; pulsado=%d; antirrebote=%lu ms",
        (int)s_gpio,
        s_nivel_activo,
        (unsigned long)s_antirrebote_ms
    );

    return ESP_OK;

error_tarea:
    vTaskDelete(s_tarea);
    s_tarea = NULL;

error_cola:
    vQueueDelete(s_cola);
    s_cola = NULL;

    return error;
}

esp_err_t pulsador_obtener_estado(
    pulsador_estado_t *estado
)
{
    if (!s_iniciado) {
        return ESP_ERR_INVALID_STATE;
    }

    if (estado == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_mux);

    estado->pulsaciones = s_pulsaciones;
    estado->flancos_perdidos =
        s_flancos_perdidos;

    portEXIT_CRITICAL(&s_mux);

    return ESP_OK;
}
