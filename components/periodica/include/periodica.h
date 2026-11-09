
#ifndef T2_PERIODICA_H
#define T2_PERIODICA_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "carga.h"

typedef struct {
    bool valido;
    carga_modo_t modo;

    uint32_t activaciones;
    uint32_t intervalos;
    uint32_t rebases;
    uint32_t errores_sensor;

    double medio_ms;
    double minimo_ms;
    double maximo_ms;
    double jitter_ms;
} periodica_resumen_t;

esp_err_t periodica_iniciar(uint32_t periodo_ms);

esp_err_t periodica_ultimo_resumen(
    carga_modo_t modo,
    periodica_resumen_t *resumen
);

#endif
