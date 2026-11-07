
#ifndef T1_PULSADOR_H
#define T1_PULSADOR_H

#include <stdint.h>
#include "driver/gpio.h"
#include "esp_err.h"

typedef struct {
    gpio_num_t gpio;
    int nivel_activo;
    uint32_t antirrebote_ms;
} pulsador_config_t;

typedef struct {
    uint32_t pulsaciones;
    uint32_t flancos_perdidos;
} pulsador_estado_t;

/*
 * Callback ejecutado desde la tarea del pulsador,
 * despues de confirmar una pulsacion valida.
 * Nunca se ejecuta en contexto de interrupcion.
 */
typedef void (*pulsador_callback_t)(void *contexto);

/*
 * Registrar antes de llamar a pulsador_iniciar().
 * Solo se utiliza para pulsaciones confirmadas.
 */
esp_err_t pulsador_registrar_callback(
    pulsador_callback_t callback,
    void *contexto
);

esp_err_t pulsador_iniciar(
    const pulsador_config_t *config
);

esp_err_t pulsador_obtener_estado(
    pulsador_estado_t *estado
);

#endif
