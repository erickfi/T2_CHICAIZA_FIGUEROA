
#ifndef T2_LED_H
#define T2_LED_H

#include <stdint.h>
#include "driver/gpio.h"
#include "esp_err.h"

/* Modos funcionales de P3. */
typedef enum {
    LED_MODO_APAGADO = 0,
    LED_MODO_INTERMEDIO,
    LED_MODO_ENCENDIDO,
    LED_MODO_TRANSICION
} led_modo_t;

/* Configuracion electrica proporcionada por main.c. */
typedef struct {
    gpio_num_t gpio;
    int nivel_encendido;  /* 0: activo bajo; 1: activo alto. */
} led_config_t;

/* Estado logico del componente, no una lectura del PWM. */
typedef struct {
    led_modo_t modo;
    uint32_t duty_objetivo;
    uint32_t frecuencia_hz;
    uint8_t resolucion_bits;
} led_estado_t;

/* Inicializa LEDC y crea la unica tarea propietaria. */
esp_err_t led_iniciar(const led_config_t *config);

/* Encola un cambio de modo sin acceder directamente a LEDC. */
esp_err_t led_fijar_modo(led_modo_t modo);

/* Encola el avance al siguiente modo. */
esp_err_t led_siguiente_modo(void);

/* Obtiene una copia coherente del estado logico. */
esp_err_t led_obtener_estado(led_estado_t *estado);

#endif
