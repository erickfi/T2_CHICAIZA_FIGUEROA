#ifndef T1_PULSADOR_H
#define T1_PULSADOR_H

#include <stdint.h>
#include "driver/gpio.h"
#include "esp_err.h"

typedef struct {
    gpio_num_t gpio;
    int nivel_activo;           /* 0 = pulsado en bajo; 1 = pulsado en alto. */
    uint32_t antirrebote_ms;   /* Ventana justificada mediante medidas. */
} pulsador_config_t;

typedef struct {
    uint32_t pulsaciones;
    uint32_t flancos_perdidos; /* Cola llena en la ISR. */
} pulsador_estado_t;

esp_err_t pulsador_iniciar(const pulsador_config_t *config);
esp_err_t pulsador_obtener_estado(pulsador_estado_t *estado);

#endif
