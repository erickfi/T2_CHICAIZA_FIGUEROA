
#include "led.h"

#include <stdbool.h>
#include <stdint.h>

#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "LEDC";

/* Configuracion de P3. */
#define LEDC_FRECUENCIA_HZ       5000U
#define LEDC_RESOLUCION_BITS    13U
#define LEDC_DUTY_TOTAL         (1U << LEDC_RESOLUCION_BITS)
#define LEDC_TIEMPO_FADE_MS     1500U

#define LEDC_MODO              LEDC_LOW_SPEED_MODE
#define LEDC_TEMPORIZADOR      LEDC_TIMER_0
#define LEDC_CANAL             LEDC_CHANNEL_0

/* Recursos FreeRTOS. */
#define LED_COLA_LONGITUD      12U
#define LED_TAREA_PILA_BYTES   4096U
#define LED_TAREA_PRIORIDAD    5U


typedef enum {
    ORDEN_FIJAR_MODO,
    ORDEN_SIGUIENTE_MODO,
    ORDEN_FIJAR_DUTY
} led_orden_tipo_t;

typedef struct {
    led_orden_tipo_t tipo;
    led_modo_t modo;
    uint32_t porcentaje;
} led_orden_t;


static QueueHandle_t s_cola = NULL;
static SemaphoreHandle_t s_mutex_estado = NULL;

/* Solo la tarea LEDC modifica estas variables. */
static led_modo_t s_modo = LED_MODO_APAGADO;
static uint32_t s_duty_objetivo = 0U;

static const char *nombre_modo(led_modo_t modo)
{
    switch (modo) {
        case LED_MODO_APAGADO:
            return "apagado";
        case LED_MODO_INTERMEDIO:
            return "intermedio";
        case LED_MODO_ENCENDIDO:
            return "encendido";
        case LED_MODO_TRANSICION:
            return "transicion";
        case LED_MODO_MANUAL:
            return "manual";
        default:
            return "desconocido";
    }
}

static led_modo_t siguiente_modo(led_modo_t actual)
{
    switch (actual) {
        case LED_MODO_APAGADO:
            return LED_MODO_INTERMEDIO;
        case LED_MODO_INTERMEDIO:
            return LED_MODO_ENCENDIDO;
        case LED_MODO_ENCENDIDO:
            return LED_MODO_TRANSICION;
        case LED_MODO_TRANSICION:
        default:
            return LED_MODO_APAGADO;
    }
}

/*
 * Funcion privada. Solo la invoca la tarea LEDC.
 * Detener el fade evita que una escritura de duty
 * tenga que esperar a que termine la transicion.
 */
static esp_err_t fijar_duty(uint32_t duty)
{
    esp_err_t err = ledc_fade_stop(
        LEDC_MODO,
        LEDC_CANAL
    );

    if (err != ESP_OK) {
        return err;
    }

    return ledc_set_duty_and_update(
        LEDC_MODO,
        LEDC_CANAL,
        duty,
        0
    );
}

/*
 * Transicion 0 % -> 100 %.
 * El hardware realiza los pasos autonomamente.
 */
static esp_err_t iniciar_transicion(void)
{
    /* Apagar el LED y detener el fade anterior. */
    esp_err_t err = fijar_duty(0U);

    if (err != ESP_OK) {
        return err;
    }

    /* Mantener el LED apagado para hacer
       perceptible el inicio de la transicion. */
    vTaskDelay(pdMS_TO_TICKS(400));

    /* Iniciar el aumento progresivo de brillo. */
    return ledc_set_fade_time_and_start(
        LEDC_MODO,
        LEDC_CANAL,
        LEDC_DUTY_TOTAL,
        LEDC_TIEMPO_FADE_MS,
        LEDC_FADE_NO_WAIT
    );
}

/*
 * Aplica un modo y actualiza el estado solo si
 * las operaciones del controlador han tenido exito.
 */
static esp_err_t aplicar_modo(led_modo_t modo)
{
    esp_err_t err = ESP_OK;
    uint32_t objetivo = 0U;

    switch (modo) {
        case LED_MODO_APAGADO:
            objetivo = 0U;
            err = fijar_duty(objetivo);
            break;

        case LED_MODO_INTERMEDIO:
            objetivo = LEDC_DUTY_TOTAL / 2U;
            err = fijar_duty(objetivo);
            break;

        case LED_MODO_ENCENDIDO:
            objetivo = LEDC_DUTY_TOTAL;
            err = fijar_duty(objetivo);
            break;

        case LED_MODO_TRANSICION:
            objetivo = LEDC_DUTY_TOTAL;
            err = iniciar_transicion();
            break;

        default:
            return ESP_ERR_INVALID_ARG;
    }

    if (err != ESP_OK) {
        return err;
    }

    /* Se protege la copia que consultan otras tareas. */
    xSemaphoreTake(s_mutex_estado, portMAX_DELAY);

    s_modo = modo;
    s_duty_objetivo = objetivo;

    xSemaphoreGive(s_mutex_estado);

    ESP_LOGI(
        TAG,
        "Modo: %s; duty objetivo: %lu/%lu",
        nombre_modo(modo),
        (unsigned long)objetivo,
        (unsigned long)LEDC_DUTY_TOTAL
    );

    return ESP_OK;
}


/*
 * P4: aplica un porcentaje solicitado desde consola.
 * Solo la tarea LEDC ejecuta esta funcion.
 */
static esp_err_t aplicar_duty_porcentaje(
    uint32_t porcentaje
)
{
    if (porcentaje > 100U) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Conversion de porcentaje a pasos de 13 bits. */
    const uint32_t duty =
        (porcentaje * LEDC_DUTY_TOTAL + 50U) / 100U;

    /*
     * fijar_duty() ya llama a ledc_fade_stop()
     * antes de actualizar el canal.
     */
    esp_err_t err = fijar_duty(duty);

    if (err != ESP_OK) {
        return err;
    }

    xSemaphoreTake(s_mutex_estado, portMAX_DELAY);

    s_modo = LED_MODO_MANUAL;
    s_duty_objetivo = duty;

    xSemaphoreGive(s_mutex_estado);

    ESP_LOGI(
        TAG,
        "P4: duty manual=%lu%% (%lu/%lu)",
        (unsigned long)porcentaje,
        (unsigned long)duty,
        (unsigned long)LEDC_DUTY_TOTAL
    );

    return ESP_OK;
}


/*
 * Unica tarea que controla el canal LEDC.
 * No realiza espera activa: se bloquea en la cola.
 */

static void tarea_ledc(void *argumento)
{
    (void)argumento;

    led_orden_t orden;

    for (;;) {

        if (xQueueReceive(
                s_cola,
                &orden,
                portMAX_DELAY
            ) != pdTRUE) {
            continue;
        }

        esp_err_t err;

        if (orden.tipo == ORDEN_FIJAR_DUTY) {

            /* P4: duty solicitado desde consola. */
            err = aplicar_duty_porcentaje(
                orden.porcentaje
            );

        } else {

            led_modo_t destino;

            if (orden.tipo == ORDEN_SIGUIENTE_MODO) {

                destino = siguiente_modo(s_modo);

            } else if (orden.tipo == ORDEN_FIJAR_MODO) {

                destino = orden.modo;

            } else {

                ESP_LOGW(TAG, "Orden LED desconocida");
                continue;
            }

            /* P3: modos originales. */
            err = aplicar_modo(destino);
        }

        if (err != ESP_OK) {
            ESP_LOGE(
                TAG,
                "Error aplicando orden LED: %s",
                esp_err_to_name(err)
            );
        }
    }
}


esp_err_t led_iniciar(const led_config_t *config)
{
    if (config == NULL ||
        (config->nivel_encendido != 0 &&
         config->nivel_encendido != 1)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_cola != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * El temporizador necesita:
     * 5000 * 8192 = 40,96 MHz.
     */
    ledc_timer_config_t temporizador = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_13_BIT,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK
    };

    esp_err_t err = ledc_timer_config(&temporizador);

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Error configurando temporizador: %s",
            esp_err_to_name(err)
        );
        return err;
    }

    /*
     * El canal LEDC ocupa el GPIO del LED.
     * No se configura tambien como GPIO digital.
     *
     * output_invert permite respetar un montaje
     * con LED activo en nivel bajo.
     */
    ledc_channel_config_t canal = {
        .gpio_num = config->gpio,
        .speed_mode = LEDC_MODO,
        .channel = LEDC_CANAL,
        .timer_sel = LEDC_TEMPORIZADOR,
        .duty = 0U,
        .hpoint = 0,
        .flags.output_invert =
            (config->nivel_encendido == 0)
    };

    err = ledc_channel_config(&canal);

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Error configurando canal: %s",
            esp_err_to_name(err)
        );
        return err;
    }

    /*
     * El servicio de fade se instala una vez.
     * El resto de llamadas a LEDC seran
     * realizadas por la tarea propietaria.
     */
    err = ledc_fade_func_install(0);

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Error instalando fade: %s",
            esp_err_to_name(err)
        );
        return err;
    }

    s_mutex_estado = xSemaphoreCreateMutex();

    if (s_mutex_estado == NULL) {
        err = ESP_ERR_NO_MEM;
        goto error_fade;
    }

    s_cola = xQueueCreate(
        LED_COLA_LONGITUD,
        sizeof(led_orden_t)
    );

    if (s_cola == NULL) {
        err = ESP_ERR_NO_MEM;
        goto error_mutex;
    }

    s_modo = LED_MODO_APAGADO;
    s_duty_objetivo = 0U;

    if (xTaskCreate(
            tarea_ledc,
            "tarea_ledc",
            LED_TAREA_PILA_BYTES,
            NULL,
            LED_TAREA_PRIORIDAD,
            NULL
        ) != pdPASS) {
        err = ESP_ERR_NO_MEM;
        goto error_cola;
    }

    ESP_LOGI(
        TAG,
        "LEDC GPIO=%d, PWM=%lu Hz, resolucion=%u bits",
        (int)config->gpio,
        (unsigned long)LEDC_FRECUENCIA_HZ,
        (unsigned)LEDC_RESOLUCION_BITS
    );

    return ESP_OK;

error_cola:
    vQueueDelete(s_cola);
    s_cola = NULL;

error_mutex:
    vSemaphoreDelete(s_mutex_estado);
    s_mutex_estado = NULL;

error_fade:
    ledc_fade_func_uninstall();
    return err;
}

/* Todas las ordenes entran en la misma cola. */
static esp_err_t enviar_orden(const led_orden_t *orden)
{
    if (s_cola == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xQueueSend(s_cola, orden, 0) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

esp_err_t led_fijar_modo(led_modo_t modo)
{
    if (modo < LED_MODO_APAGADO ||
        modo > LED_MODO_TRANSICION) {
        return ESP_ERR_INVALID_ARG;
    }

    led_orden_t orden = {
        .tipo = ORDEN_FIJAR_MODO,
        .modo = modo
    };

    return enviar_orden(&orden);
}

esp_err_t led_siguiente_modo(void)
{
    led_orden_t orden = {
        .tipo = ORDEN_SIGUIENTE_MODO,
        .modo = LED_MODO_APAGADO
    };

    return enviar_orden(&orden);
}

esp_err_t led_obtener_estado(led_estado_t *estado)
{
    if (estado == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_mutex_estado == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex_estado, portMAX_DELAY);

    estado->modo = s_modo;
    estado->duty_objetivo = s_duty_objetivo;
    estado->frecuencia_hz = LEDC_FRECUENCIA_HZ;
    estado->resolucion_bits = LEDC_RESOLUCION_BITS;

    xSemaphoreGive(s_mutex_estado);

    return ESP_OK;
}


esp_err_t led_fijar_duty_porcentaje(
    uint32_t porcentaje
)
{
    if (porcentaje > 100U) {
        return ESP_ERR_INVALID_ARG;
    }

    led_orden_t orden = {
        .tipo = ORDEN_FIJAR_DUTY,
        .modo = LED_MODO_MANUAL,
        .porcentaje = porcentaje
    };

    return enviar_orden(&orden);
}
