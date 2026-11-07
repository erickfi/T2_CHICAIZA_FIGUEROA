#ifndef T1_LED_H
#define T1_LED_H

#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "esp_err.h"

/* Modos funcionales: distintos del nivel electrico del GPIO. */
typedef enum {
    LED_MODO_APAGADO = 0,
    LED_MODO_ENCENDIDO,
    LED_MODO_PARPADEO,
} led_modo_t;

typedef struct {
    gpio_num_t gpio;
    int nivel_encendido;       /* 0 o 1; nivel de apagado = contrario. */
    uint32_t periodo_ms;      /* Periodo completo, debe ser par. */
} led_config_t;

typedef struct {
    led_modo_t modo;
    bool encendido;
    uint32_t periodo_ms;
} led_estado_t;

/* Inicializa y arranca parpadeando (P1A). Llamar una sola vez. */
esp_err_t led_iniciar(const led_config_t *config);

/* Modifica el modo sin acceder directamente al GPIO desde otro modulo. */
esp_err_t led_fijar_modo(led_modo_t modo);

/* Copia coherente del estado; no devuelve variables internas. */
esp_err_t led_obtener_estado(led_estado_t *estado);

#endif
