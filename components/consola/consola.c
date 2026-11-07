
#include "consola.h"

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "sdkconfig.h"
#include "esp_log.h"

#include "led.h"
#include "pulsador.h"
#include "bme280.h"

#define CONSOLA_UART UART_NUM_0
#define CONSOLA_RX_BUFFER 2048
#define CONSOLA_TX_BUFFER 2048
#define CONSOLA_LINEA_MAX 96
#define CONSOLA_ARG_MAX 4

static const char *TAG = "CONSOLA";
static bool s_iniciada = false;

/* Toda salida de consola utiliza UART0. */
static void enviar(const char *texto)
{
    uart_write_bytes(
        CONSOLA_UART,
        texto,
        strlen(texto)
    );
}

static void enviar_formato(
    const char *formato,
    ...
)
{
    char mensaje[192];
    va_list argumentos;

    va_start(argumentos, formato);

    vsnprintf(
        mensaje,
        sizeof(mensaje),
        formato,
        argumentos
    );

    va_end(argumentos);

    enviar(mensaje);
}

typedef void (*manejador_t)(
    int argc,
    char *argv[]
);

typedef struct {
    const char *nombre;
    const char *uso;
    manejador_t ejecutar;
} comando_t;

static void comando_ayuda(
    int argc, char *argv[]
);

static void comando_estado(
    int argc, char *argv[]
);

static void comando_led(
    int argc, char *argv[]
);

/* Tabla centralizada de comandos. */
static const comando_t comandos[] = {
    {
        "ayuda",
        "ayuda                 Lista los comandos",
        comando_ayuda
    },
    {
        "estado",
        "estado                Muestra LED, pulsador y sensor",
        comando_estado
    },
    {
        "led",
        "led off|medio|on|fade|next   Controla LEDC",
        comando_led
    }
};

static void comando_ayuda(
    int argc,
    char *argv[]
)
{
    (void)argv;

    if (argc != 1) {
        enviar("Uso: ayuda\r\n");
        return;
    }

    for (size_t i = 0;
         i < sizeof(comandos) / sizeof(comandos[0]);
         ++i) {

        enviar(comandos[i].uso);
        enviar("\r\n");
    }
}

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

        default:
            return "desconocido";
    }
}

static void comando_estado(
    int argc,
    char *argv[]
)
{
    (void)argv;

    if (argc != 1) {
        enviar("Uso: estado\r\n");
        return;
    }

    led_estado_t led;
    pulsador_estado_t pulsador;

    if (led_obtener_estado(&led) != ESP_OK ||
        pulsador_obtener_estado(&pulsador) != ESP_OK) {

        enviar("Error: estado no disponible\r\n");
        return;
    }

    /* Estado logico de LEDC. */
    enviar_formato(
        "LEDC: modo=%s; objetivo=%" PRIu32
        " pasos; PWM=%" PRIu32 " Hz; %u bits\r\n",
        nombre_modo(led.modo),
        led.duty_objetivo,
        led.frecuencia_hz,
        (unsigned)led.resolucion_bits
    );

    enviar_formato(
        "Pulsaciones: %" PRIu32
        "; flancos perdidos: %" PRIu32 "\r\n",
        pulsador.pulsaciones,
        pulsador.flancos_perdidos
    );

    /* Se conserva la consulta del sensor del T1. */
    bme280_estado_t sensor;

    esp_err_t error_sensor =
        bme280_obtener_estado(&sensor);

    if (error_sensor != ESP_OK) {

        enviar("Sensor: no disponible\r\n");

    } else if (!sensor.muestra_valida) {

        enviar("Sensor: sin muestra valida\r\n");

    } else {

        enviar_formato(
            "Temperatura: %.2f C\r\n",
            (double)sensor.temperatura_c
        );

        enviar_formato(
            "Presion: %.2f hPa\r\n",
            (double)sensor.presion_hpa
        );

        if (sensor.tiene_humedad) {

            enviar_formato(
                "Humedad: %.2f %%HR\r\n",
                (double)sensor.humedad_pct
            );

        } else {

            enviar(
                "Humedad: no disponible (BMP280)\r\n"
            );
        }
    }
}

/*
 * P3: La consola solamente envia ordenes.
 * No llama directamente a driver/ledc.h.
 */
static void comando_led(
    int argc,
    char *argv[]
)
{
    if (argc != 2) {
        enviar("Uso: led off|medio|on|fade|next\r\n");
        return;
    }

    esp_err_t error;

    if (strcmp(argv[1], "next") == 0) {

        error = led_siguiente_modo();

    } else {

        led_modo_t modo;

        if (strcmp(argv[1], "off") == 0) {

            modo = LED_MODO_APAGADO;

        } else if (strcmp(argv[1], "medio") == 0) {

            modo = LED_MODO_INTERMEDIO;

        } else if (strcmp(argv[1], "on") == 0) {

            modo = LED_MODO_ENCENDIDO;

        } else if (strcmp(argv[1], "fade") == 0) {

            modo = LED_MODO_TRANSICION;

        } else {

            enviar(
                "Uso: led off|medio|on|fade|next\r\n"
            );
            return;
        }

        error = led_fijar_modo(modo);
    }

    if (error == ESP_OK) {

        enviar("LEDC: orden encolada\r\n");

    } else {

        enviar_formato(
            "LEDC: error %s\r\n",
            esp_err_to_name(error)
        );
    }
}

/* Analizador de lineas existente. */
static void ejecutar_linea(char *linea)
{
    char *argv[CONSOLA_ARG_MAX];
    int argc = 0;

    char *contexto = NULL;
    char *token = strtok_r(
        linea,
        " \t",
        &contexto
    );

    while (token != NULL) {

        if (argc >= CONSOLA_ARG_MAX) {

            enviar(
                "Error: demasiados argumentos\r\n"
            );
            return;
        }

        argv[argc++] = token;

        token = strtok_r(
            NULL,
            " \t",
            &contexto
        );
    }

    if (argc == 0) {
        return;
    }

    for (size_t i = 0;
         i < sizeof(comandos) / sizeof(comandos[0]);
         ++i) {

        if (strcmp(
                argv[0],
                comandos[i].nombre
            ) == 0) {

            comandos[i].ejecutar(argc, argv);
            return;
        }
    }

    enviar_formato(
        "Comando desconocido: %s (escribe ayuda)\r\n",
        argv[0]
    );
}

/* Tarea de consola, conservada del Taller 1. */
static void tarea_consola(void *argumento)
{
    (void)argumento;

    char linea[CONSOLA_LINEA_MAX + 1U];
    size_t longitud = 0;

    bool desbordada = false;
    bool anterior_cr = false;

    uint8_t caracter;

    enviar(
        "\r\nConsola del Taller 2. Escribe 'ayuda'.\r\nT2> "
    );

    while (true) {

        if (uart_read_bytes(
                CONSOLA_UART,
                &caracter,
                1,
                portMAX_DELAY
            ) != 1) {
            continue;
        }

        if (caracter == '\r' || caracter == '\n') {

            if (caracter == '\n' && anterior_cr) {
                anterior_cr = false;
                continue;
            }

            anterior_cr = (caracter == '\r');

            enviar("\r\n");

            if (desbordada) {

                enviar(
                    "Error: linea demasiado larga; "
                    "descartada completa\r\n"
                );

            } else {

                linea[longitud] = '\0';
                ejecutar_linea(linea);
            }

            longitud = 0;
            desbordada = false;

            enviar("T2> ");
            continue;
        }

        anterior_cr = false;

        if (desbordada) {
            continue;
        }

        if (longitud >= CONSOLA_LINEA_MAX) {
            desbordada = true;
        } else {
            linea[longitud++] = (char)caracter;
        }
    }
}

esp_err_t consola_iniciar(void)
{
    if (s_iniciada) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t error = uart_driver_install(
        CONSOLA_UART,
        CONSOLA_RX_BUFFER,
        CONSOLA_TX_BUFFER,
        0,
        NULL,
        0
    );

    if (error != ESP_OK) {
        return error;
    }

    const uart_config_t cfg = {
        .baud_rate = CONFIG_ESP_CONSOLE_UART_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT
    };

    error = uart_param_config(CONSOLA_UART, &cfg);

    if (error != ESP_OK) {
        goto error_driver;
    }

    error = uart_set_pin(
        CONSOLA_UART,
        UART_PIN_NO_CHANGE,
        UART_PIN_NO_CHANGE,
        UART_PIN_NO_CHANGE,
        UART_PIN_NO_CHANGE
    );

    if (error != ESP_OK) {
        goto error_driver;
    }

    uart_vfs_dev_use_driver(CONSOLA_UART);

    if (xTaskCreate(
            tarea_consola,
            "tarea_consola",
            4096,
            NULL,
            4,
            NULL
        ) != pdPASS) {

        error = ESP_ERR_NO_MEM;
        goto error_driver;
    }

    s_iniciada = true;

    ESP_LOGI(
        TAG,
        "UART0: %d baudios; 8N1; linea maxima %d",
        CONFIG_ESP_CONSOLE_UART_BAUDRATE,
        CONSOLA_LINEA_MAX
    );

    return ESP_OK;

error_driver:
    uart_driver_delete(CONSOLA_UART);
    return error;
}
