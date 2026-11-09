
#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bme280.h"
#include "carga.h"
#include "consola.h"
#include "led.h"
#include "periodica.h"
#include "pines.h"
#include "pulsador.h"

static const char *TAG = "MAIN";

/*
 * Callback P3.
 * Ejecutado desde la tarea del pulsador,
 * despues del antirrebote; nunca desde ISR.
 */
static void pulsacion_validada(void *contexto)
{
    (void)contexto;

    esp_err_t err = led_siguiente_modo();

    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Orden LED perdida: %s",
            esp_err_to_name(err)
        );
    }
}

void app_main(void)
{
    /* P3: LEDC */
    const led_config_t led_cfg = {
        .gpio = PIN_LED,
        .nivel_encendido = LED_NIVEL_ENCENDIDO
    };

    ESP_ERROR_CHECK(led_iniciar(&led_cfg));

    /* P3: pulsador y callback. */
    ESP_ERROR_CHECK(
        pulsador_registrar_callback(
            pulsacion_validada, NULL
        )
    );

    const pulsador_config_t pulsador_cfg = {
        .gpio = PIN_PULSADOR,
        .nivel_activo = PULSADOR_NIVEL_ACTIVO,
        .antirrebote_ms = PULSADOR_ANTIRREBOTE_MS
    };

    ESP_ERROR_CHECK(
        pulsador_iniciar(&pulsador_cfg)
    );

    /*
     * P1: crear tareas de carga.
     * El modo inicial es PARADA.
     * P2 necesita consultar este componente.
     */
    ESP_ERROR_CHECK(carga_iniciar());

    /* Sensor compartido con P2. */
    const bme280_config_t sensor_cfg = {
        .sda = PIN_BME280_SDA,
        .scl = PIN_BME280_SCL,
        .direccion = BME280_DIRECCION,
        .periodo_ms = BME280_PERIODO_MS
    };

    const esp_err_t err_sensor =
        bme280_iniciar(&sensor_cfg);

    if (err_sensor == ESP_OK) {

        /*
         * P2: una unica tarea CPU0 realiza
         * las lecturas y mide el jitter.
         */
        ESP_ERROR_CHECK(
            periodica_iniciar(BME280_PERIODO_MS)
        );

    } else {

        ESP_LOGW(
            TAG,
            "BME280 no disponible: %s",
            esp_err_to_name(err_sensor)
        );
    }

    /* Consola comun P1, P2 y P3. */
    ESP_ERROR_CHECK(consola_iniciar());

    /* Comprobacion inicial de lectura real. */
    if (err_sensor == ESP_OK) {

        bool obtenido = false;

        for (unsigned intento = 0;
             intento < 30;
             ++intento) {

            vTaskDelay(pdMS_TO_TICKS(100));

            bme280_estado_t muestra = {0};

            if (bme280_obtener_estado(
                    &muestra
                ) != ESP_OK ||
                !muestra.muestra_valida) {
                continue;
            }

            ESP_LOGI(
                TAG,
                "Primera lectura: T=%.2f C, P=%.2f hPa",
                (double)muestra.temperatura_c,
                (double)muestra.presion_hpa
            );

            if (muestra.tiene_humedad) {
                ESP_LOGI(
                    TAG,
                    "Primera lectura: HR=%.2f %%",
                    (double)muestra.humedad_pct
                );
            }

            obtenido = true;
            break;
        }

        if (!obtenido) {
            ESP_LOGW(
                TAG,
                "No hay muestra valida: revisar BME280 e I2C"
            );
        }
    }
}
