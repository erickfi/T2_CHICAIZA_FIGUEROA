
#include "consola.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
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
#include "carga.h"
#include "periodica.h"
#include "temporizadores.h"

#define CONSOLA_UART UART_NUM_0
#define CONSOLA_RX_BUFFER 2048
#define CONSOLA_TX_BUFFER 2048
#define CONSOLA_LINEA_MAX 96
#define CONSOLA_ARG_MAX 4

static const char *TAG = "CONSOLA";
static bool s_iniciada = false;

/* Salida por UART0. */
static void enviar(const char *texto)
{
    uart_write_bytes(CONSOLA_UART, texto, strlen(texto));
}

static void enviar_formato(const char *formato, ...)
{
    char mensaje[256];
    va_list ap;

    va_start(ap, formato);
    vsnprintf(mensaje, sizeof(mensaje), formato, ap);
    va_end(ap);

    enviar(mensaje);
}

/* Convierte un decimal sin admitir signos ni desbordamiento. */
static bool leer_entero_u32(
    const char *texto,
    uint32_t *valor
)
{
    if (texto == NULL ||
        texto[0] == '\0' ||
        valor == NULL) {
        return false;
    }

    uint32_t n = 0U;

    for (; *texto != '\0'; ++texto) {

        if (*texto < '0' || *texto > '9') {
            return false;
        }

        uint32_t digito =
            (uint32_t)(*texto - '0');

        if (n > (UINT32_MAX - digito) / 10U) {
            return false;
        }

        n = n * 10U + digito;
    }

    *valor = n;
    return true;
}

typedef void (*manejador_t)(int argc, char *argv[]);

typedef struct {
    const char *nombre;
    const char *ayuda;
    manejador_t ejecutar;
} comando_t;

static void comando_ayuda(int, char *[]);
static void comando_estado(int, char *[]);
static void comando_led(int, char *[]);
static void comando_carga(int, char *[]);
static void comando_periodica(int, char *[]);
static void comando_timers(int, char *[]);
static void comando_tareas(int, char *[]);

/* Tabla unica; todas las entradas terminan en coma. */
static const comando_t comandos[] = {
    {
        "ayuda",
        "ayuda                         Lista los comandos",
        comando_ayuda
    },
    {
        "estado",
        "estado                        LED, pulsador y BME280",
        comando_estado
    },
    {
        "led",
        "led off|medio|on|fade|next|duty N  LEDC, N=0..100",
        comando_led
    },
    {
        "carga",
        "carga stop|turnos|inanicion|corregida  P1",
        comando_carga
    },
    {
        "periodica",
        "periodica                     Resumenes del sensor P2",
        comando_periodica
    },
    {
        "timers",
        "timers resumen|periodo N       P4, N=5..1000 ms",
        comando_timers
    },
    {
        "tareas",
        "tareas                        Lista y uso de CPU (6.5)",
        comando_tareas
    },
};

/* ---------------------------------------------------------
 * Nombres de los modos.
 * --------------------------------------------------------- */

static const char *nombre_led(led_modo_t modo)
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

static const char *nombre_carga(carga_modo_t modo)
{
    switch (modo) {
        case CARGA_PARADA:
            return "parada";

        case CARGA_TURNOS:
            return "turnos";

        case CARGA_INANICION:
            return "inanicion";

        case CARGA_CORREGIDA:
            return "corregida";

        default:
            return "desconocido";
    }
}

/* ---------------------------------------------------------
 * AYUDA
 * --------------------------------------------------------- */

static void comando_ayuda(int argc, char *argv[])
{
    (void)argv;

    if (argc != 1) {
        enviar("Uso: ayuda\r\n");
        return;
    }

    for (size_t i = 0;
         i < sizeof(comandos) / sizeof(comandos[0]);
         ++i) {

        enviar(comandos[i].ayuda);
        enviar("\r\n");
    }
}

/* ---------------------------------------------------------
 * ESTADO
 * --------------------------------------------------------- */

static void comando_estado(int argc, char *argv[])
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

        enviar(
            "Error: estado de LED o pulsador no disponible\r\n"
        );
        return;
    }

    enviar_formato(
        "LEDC: modo=%s objetivo=%" PRIu32
        " pasos PWM=%" PRIu32 " Hz, %u bits\r\n",
        nombre_led(led.modo),
        led.duty_objetivo,
        led.frecuencia_hz,
        (unsigned)led.resolucion_bits
    );

    enviar_formato(
        "Pulsaciones=%" PRIu32
        "; flancos perdidos=%" PRIu32 "\r\n",
        pulsador.pulsaciones,
        pulsador.flancos_perdidos
    );

    bme280_estado_t sensor;
    esp_err_t err = bme280_obtener_estado(&sensor);

    if (err != ESP_OK) {

        enviar("Sensor: no disponible\r\n");

    } else if (!sensor.muestra_valida) {

        enviar("Sensor: sin muestra valida\r\n");

    } else {

        enviar_formato(
            "Temperatura=%.2f C, presion=%.2f hPa\r\n",
            (double)sensor.temperatura_c,
            (double)sensor.presion_hpa
        );

        if (sensor.tiene_humedad) {
            enviar_formato(
                "Humedad=%.2f %%HR\r\n",
                (double)sensor.humedad_pct
            );
        } else {
            enviar(
                "Humedad: no disponible (BMP280)\r\n"
            );
        }
    }

    uint32_t periodo;

    if (temporizadores_obtener_periodo(&periodo)
        == ESP_OK) {

        enviar_formato(
            "P4: temporizadores=%" PRIu32 " ms\r\n",
            periodo
        );
    }
}

/* ---------------------------------------------------------
 * LEDC - P3 y P4
 * --------------------------------------------------------- */

static void comando_led(int argc, char *argv[])
{
    const char *uso =
        "Uso: led off|medio|on|fade|next|duty N "
        "(N=0..100)\r\n";

    esp_err_t err;

    if (argc == 3 &&
        strcmp(argv[1], "duty") == 0) {

        uint32_t porcentaje;

        if (!leer_entero_u32(
                argv[2],
                &porcentaje
            ) || porcentaje > 100U) {

            enviar(uso);
            return;
        }

        err = led_fijar_duty_porcentaje(porcentaje);

    } else if (argc == 2) {

        if (strcmp(argv[1], "next") == 0) {
            err = led_siguiente_modo();

        } else if (strcmp(argv[1], "off") == 0) {
            err = led_fijar_modo(LED_MODO_APAGADO);

        } else if (strcmp(argv[1], "medio") == 0) {
            err = led_fijar_modo(LED_MODO_INTERMEDIO);

        } else if (strcmp(argv[1], "on") == 0) {
            err = led_fijar_modo(LED_MODO_ENCENDIDO);

        } else if (strcmp(argv[1], "fade") == 0) {
            err = led_fijar_modo(LED_MODO_TRANSICION);

        } else {
            enviar(uso);
            return;
        }

    } else {
        enviar(uso);
        return;
    }

    if (err == ESP_OK) {
        enviar("LEDC: orden encolada\r\n");
    } else {
        enviar_formato(
            "LEDC: error %s\r\n",
            esp_err_to_name(err)
        );
    }
}

/* ---------------------------------------------------------
 * CARGA - P1
 * --------------------------------------------------------- */

static void comando_carga(int argc, char *argv[])
{
    if (argc != 2) {
        enviar(
            "Uso: carga stop|turnos|inanicion|corregida\r\n"
        );
        return;
    }

    carga_modo_t modo;

    if (strcmp(argv[1], "stop") == 0) {
        modo = CARGA_PARADA;

    } else if (strcmp(argv[1], "turnos") == 0) {
        modo = CARGA_TURNOS;

    } else if (strcmp(argv[1], "inanicion") == 0) {
        modo = CARGA_INANICION;

    } else if (strcmp(argv[1], "corregida") == 0) {
        modo = CARGA_CORREGIDA;

    } else {
        enviar("Modo de carga desconocido\r\n");
        return;
    }

    esp_err_t err = carga_fijar_modo(modo);

    if (err == ESP_OK) {
        enviar_formato(
            "Carga: modo %s solicitado\r\n",
            nombre_carga(modo)
        );
    } else {
        enviar_formato(
            "Carga: error %s\r\n",
            esp_err_to_name(err)
        );
    }
}

/* ---------------------------------------------------------
 * PERIODICA - P2
 * --------------------------------------------------------- */

static void comando_periodica(int argc, char *argv[])
{
    (void)argv;

    if (argc != 1) {
        enviar("Uso: periodica\r\n");
        return;
    }

    for (int m = CARGA_PARADA;
         m <= CARGA_CORREGIDA;
         ++m) {

        periodica_resumen_t r = {0};

        esp_err_t err = periodica_ultimo_resumen(
            (carga_modo_t)m,
            &r
        );

        if (err == ESP_ERR_NOT_FOUND) {
            enviar_formato(
                "%s: sin resumen completo\r\n",
                nombre_carga((carga_modo_t)m)
            );
            continue;
        }

        if (err != ESP_OK) {
            enviar_formato(
                "%s: error %s\r\n",
                nombre_carga((carga_modo_t)m),
                esp_err_to_name(err)
            );
            continue;
        }

        enviar_formato(
            "%s: n=%" PRIu32
            " intervalos=%" PRIu32
            " rebases=%" PRIu32
            " errores_sensor=%" PRIu32 "\r\n",

            nombre_carga((carga_modo_t)m),
            r.activaciones,
            r.intervalos,
            r.rebases,
            r.errores_sensor
        );

        enviar_formato(
            "  medio=%.3f min=%.3f max=%.3f"
            " jitter=%.3f ms\r\n",
            r.medio_ms,
            r.minimo_ms,
            r.maximo_ms,
            r.jitter_ms
        );
    }
}

/* ---------------------------------------------------------
 * TIMERS - P4
 * --------------------------------------------------------- */

static void comando_timers(int argc, char *argv[])
{
    /* Cambio simultaneo del periodo solicitado. */
    if (argc == 3 &&
        strcmp(argv[1], "periodo") == 0) {

        uint32_t nuevo;

        if (!leer_entero_u32(argv[2], &nuevo) ||
            nuevo < 5U ||
            nuevo > 1000U) {

            enviar(
                "Uso: timers periodo N (5..1000 ms)\r\n"
            );
            return;
        }

        esp_err_t err =
            temporizadores_fijar_periodo(nuevo);

        if (err == ESP_OK) {
            enviar_formato(
                "P4: periodo solicitado=%" PRIu32
                " ms para ambos\r\n",
                nuevo
            );
        } else {
            enviar_formato(
                "P4: error al cambiar periodo: %s\r\n",
                esp_err_to_name(err)
            );
        }

        return;
    }

    /* Consulta de ultimos resumenes completos. */
    if (argc != 2 ||
        strcmp(argv[1], "resumen") != 0) {

        enviar(
            "Uso: timers resumen | timers periodo N\r\n"
        );
        return;
    }

    uint32_t periodo;

    if (temporizadores_obtener_periodo(&periodo)
        != ESP_OK) {

        enviar("P4: no iniciado\r\n");
        return;
    }

    enviar_formato(
        "P4: periodo actual=%" PRIu32
        " ms, 50 activaciones por ventana\r\n",
        periodo
    );

    for (int fuente = TEMPORIZADOR_ESP;
         fuente <= TEMPORIZADOR_FREERTOS;
         ++fuente) {

        const char *origen =
            fuente == TEMPORIZADOR_ESP ?
                "esp_timer" : "FreeRTOS";

        uint32_t perdidos = 0;

        (void)temporizadores_perdidos(
            (temporizador_origen_t)fuente,
            &perdidos
        );

        enviar_formato(
            "%s: envios perdidos acumulados=%"
            PRIu32 "\r\n",
            origen,
            perdidos
        );

        for (int m = CARGA_PARADA;
             m <= CARGA_CORREGIDA;
             ++m) {

            temporizadores_resumen_t r = {0};

            esp_err_t err =
                temporizadores_ultimo_resumen(
                    (temporizador_origen_t)fuente,
                    (carga_modo_t)m,
                    &r
                );

            if (err == ESP_ERR_NOT_FOUND) {

                enviar_formato(
                    "  %s: sin resumen vigente\r\n",
                    nombre_carga((carga_modo_t)m)
                );
                continue;
            }

            if (err != ESP_OK) {

                enviar_formato(
                    "  %s: error %s\r\n",
                    nombre_carga((carga_modo_t)m),
                    esp_err_to_name(err)
                );
                continue;
            }

            enviar_formato(
                "  %s: n=%" PRIu32
                " medio=%.3f min=%.3f max=%.3f"
                " jitter=%.3f ms rebases=%"
                PRIu32 "\r\n",

                nombre_carga((carga_modo_t)m),
                r.activaciones,
                r.medio_ms,
                r.minimo_ms,
                r.maximo_ms,
                r.jitter_ms,
                r.rebases
            );
        }
    }
}

/* ---------------------------------------------------------
 * DIAGNOSTICO DE TAREAS - APARTADO 6.5
 * --------------------------------------------------------- */

static void comando_tareas(int argc, char *argv[])
{
    (void)argv;

    if (argc != 1) {
        enviar("Uso: tareas\r\n");
        return;
    }

#if CONFIG_FREERTOS_USE_TRACE_FACILITY && \
    CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS && \
    CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS

    /*
     * Reservar margen superior al minimo recomendado
     * de unos 40 bytes por tarea.
     */
    const size_t bytes =
        ((size_t)uxTaskGetNumberOfTasks() + 16U) * 96U;

    char *buffer = malloc(bytes);

    if (buffer == NULL) {
        enviar(
            "Tareas: memoria insuficiente para diagnostico\r\n"
        );
        return;
    }

    enviar(
        "Nombre          Estado Prio Pila Num Core\r\n"
    );

    vTaskList(buffer);
    enviar(buffer);

    enviar(
        "\r\nUso de CPU acumulado desde arranque:\r\n"
    );

    vTaskGetRunTimeStats(buffer);
    enviar(buffer);

    free(buffer);

#else

    enviar(
        "Tareas: activar trace, stats y run time "
        "stats en sdkconfig\r\n"
    );

#endif
}

/* ---------------------------------------------------------
 * ANALIZADOR DE LINEAS
 * --------------------------------------------------------- */

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

/* ---------------------------------------------------------
 * TAREA DE CONSOLA UART0
 * --------------------------------------------------------- */

static void tarea_consola(void *argumento)
{
    (void)argumento;

    char linea[CONSOLA_LINEA_MAX + 1U];
    size_t longitud = 0;

    bool desbordada = false;
    bool anterior_cr = false;

    uint8_t caracter;

    enviar(
        "\r\nConsola del Taller 2. "
        "Escribe 'ayuda'.\r\nT2> "
    );

    for (;;) {

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

/* ---------------------------------------------------------
 * INICIALIZACION UART0
 * --------------------------------------------------------- */

esp_err_t consola_iniciar(void)
{
    if (s_iniciada) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = uart_driver_install(
        CONSOLA_UART,
        CONSOLA_RX_BUFFER,
        CONSOLA_TX_BUFFER,
        0,
        NULL,
        0
    );

    if (err != ESP_OK) {
        return err;
    }

    const uart_config_t cfg = {
        .baud_rate = CONFIG_ESP_CONSOLE_UART_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT
    };

    err = uart_param_config(CONSOLA_UART, &cfg);

    if (err != ESP_OK) {
        goto error_driver;
    }

    err = uart_set_pin(
        CONSOLA_UART,
        UART_PIN_NO_CHANGE,
        UART_PIN_NO_CHANGE,
        UART_PIN_NO_CHANGE,
        UART_PIN_NO_CHANGE
    );

    if (err != ESP_OK) {
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

        err = ESP_ERR_NO_MEM;
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
    return err;
}
