
#ifndef T2_CARGA_H
#define T2_CARGA_H

#include <stdint.h>
#include "esp_err.h"

typedef enum {
    CARGA_PARADA = 0,
    CARGA_TURNOS,
    CARGA_INANICION,
    CARGA_CORREGIDA
} carga_modo_t;

typedef struct {
    carga_modo_t modo;
    uint32_t generacion;
    uint32_t contador_1;
    uint32_t contador_2;
} carga_estado_t;

esp_err_t carga_iniciar(void);
esp_err_t carga_fijar_modo(carga_modo_t nuevo);
esp_err_t carga_obtener_estado(carga_estado_t *estado);

#endif
