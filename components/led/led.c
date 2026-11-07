#include "led.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "LED";

/* Estos datos son PRIVADOS del componente. */
static SemaphoreHandle_t s_mutex = NULL;
static gpio_num_t s_gpio;
static int s_nivel_encendido;
static uint32_t s_periodo_ms;
static led_modo_t s_modo = LED_MODO_APAGADO;
static bool s_encendido = false;

static esp_err_t escribir_led(bool encender)
{
    int nivel = encender ? s_nivel_encendido : 1 - s_nivel_encendido;
    esp_err_t error = gpio_set_level(s_gpio, nivel);
    if (error == ESP_OK) {
        s_encendido = encender;
    }
    return error;
}

static void tarea_led(void *argumento)
{
    (void)argumento;
    const TickType_t medio_periodo = pdMS_TO_TICKS(s_periodo_ms / 2U);
    TickType_t ultimo_despertar = xTaskGetTickCount();

    while (true) {
        bool hay_traza = false;
        bool encendido = false;
        int64_t instante_us = 0;

        if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
            if (s_modo == LED_MODO_PARPADEO) {
                if (escribir_led(!s_encendido) == ESP_OK) {
                    instante_us = esp_timer_get_time();
                    encendido = s_encendido;
                    hay_traza = true;
                }
            }
            xSemaphoreGive(s_mutex);
        }

        /* El registro se hace en contexto de tarea y fuera del mutex. */
        if (hay_traza) {
            ESP_LOGI(TAG, "Conmutacion: %s; t=%lld us",
                     encendido ? "ON" : "OFF", (long long)instante_us);
        }

        /* Espera periodica: evita sumar el trabajo del bucle al periodo. */
        xTaskDelayUntil(&ultimo_despertar, medio_periodo);
    }
}

esp_err_t led_iniciar(const led_config_t *config)
{
    if (config == NULL || s_mutex != NULL ||
        (int)config->gpio < 0 || (int)config->gpio >= 64 ||
        (config->nivel_encendido != 0 && config->nivel_encendido != 1) ||
        config->periodo_ms < 2U || (config->periodo_ms % 2U) != 0U ||
        pdMS_TO_TICKS(config->periodo_ms / 2U) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    gpio_config_t gpio_cfg = {
        .pin_bit_mask = (1ULL << config->gpio),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t error = gpio_config(&gpio_cfg);
    if (error != ESP_OK) {
        return error;
    }

    s_gpio = config->gpio;
    s_nivel_encendido = config->nivel_encendido;
    s_periodo_ms = config->periodo_ms;
    s_modo = LED_MODO_APAGADO;

    error = escribir_led(false);
    if (error != ESP_OK) {
        return error;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_modo = LED_MODO_PARPADEO;
    if (xTaskCreate(tarea_led, "tarea_led", 3072, NULL, 5, NULL) != pdPASS) {
        s_modo = LED_MODO_APAGADO;
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "GPIO=%d; activo=%d; periodo=%lu ms",
             (int)s_gpio, s_nivel_encendido, (unsigned long)s_periodo_ms);
    return ESP_OK;
}

esp_err_t led_fijar_modo(led_modo_t modo)
{
    if (s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (modo != LED_MODO_APAGADO && modo != LED_MODO_ENCENDIDO &&
        modo != LED_MODO_PARPADEO) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_err_t error = ESP_OK;

    if (modo == LED_MODO_APAGADO) {
        error = escribir_led(false);
    } else if (modo == LED_MODO_ENCENDIDO) {
        error = escribir_led(true);
    } else if (s_modo != LED_MODO_PARPADEO) {
        /* Cada retorno al parpadeo comienza con LED apagado. */
        error = escribir_led(false);
    }

    if (error == ESP_OK) {
        s_modo = modo;
    }
    xSemaphoreGive(s_mutex);
    return error;
}

esp_err_t led_obtener_estado(led_estado_t *estado)
{
    if (s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (estado == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    estado->modo = s_modo;
    estado->encendido = s_encendido;
    estado->periodo_ms = s_periodo_ms;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}
