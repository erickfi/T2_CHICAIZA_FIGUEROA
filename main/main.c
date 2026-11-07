#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bme280.h"
#include "consola.h"
#include "led.h"
#include "pines.h"
#include "pulsador.h"

static const char *TAG = "MAIN";

/* Se ejecuta en la tarea del pulsador, despues del antirrebote; NO en ISR. */
static void pulsacion_validada(void *contexto)
{
    (void)contexto;
    esp_err_t err = led_siguiente_modo();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Orden LED perdida: %s", esp_err_to_name(err));
    }
}

void app_main(void)
{
    const led_config_t led_cfg = {
        .gpio = PIN_LED,
        .nivel_encendido = LED_NIVEL_ENCENDIDO
    };
    ESP_ERROR_CHECK(led_iniciar(&led_cfg));

    /* Registrar ANTES de iniciar la tarea del pulsador. */
    ESP_ERROR_CHECK(pulsador_registrar_callback(pulsacion_validada, NULL));
    const pulsador_config_t pulsador_cfg = {
        .gpio = PIN_PULSADOR,
        .nivel_activo = PULSADOR_NIVEL_ACTIVO,
        .antirrebote_ms = PULSADOR_ANTIRREBOTE_MS
    };
    ESP_ERROR_CHECK(pulsador_iniciar(&pulsador_cfg));

    const bme280_config_t sensor_cfg = {
        .sda = PIN_BME280_SDA,
        .scl = PIN_BME280_SCL,
        .direccion = BME280_DIRECCION,
        .periodo_ms = BME280_PERIODO_MS
    };
    ESP_ERROR_CHECK(bme280_iniciar(&sensor_cfg));
    ESP_ERROR_CHECK(consola_iniciar());

    /* Evidencia de preparatorio: una primera muestra valida, sin inventarla. */
    bool obtenido = false;
    for (unsigned intento = 0; intento < 30; ++intento) {
        vTaskDelay(pdMS_TO_TICKS(100));
        bme280_estado_t muestra = {0};
        if (bme280_obtener_estado(&muestra) != ESP_OK ||
            !muestra.muestra_valida) {
            continue;
        }
        ESP_LOGI(TAG, "Primera lectura: T=%.2f C, P=%.2f hPa",
                 (double)muestra.temperatura_c,
                 (double)muestra.presion_hpa);
        if (muestra.tiene_humedad) {
            ESP_LOGI(TAG, "Primera lectura: HR=%.2f %%",
                     (double)muestra.humedad_pct);
        }
        obtenido = true;
        break;
    }
    if (!obtenido) {
        ESP_LOGW(TAG, "No hay primera lectura valida: verificar BME280 e I2C");
    }
    /* app_main puede terminar; las tareas creadas siguen funcionando. */
}
