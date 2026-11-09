
#ifndef T2_TEMPORIZADORES_H
#define T2_TEMPORIZADORES_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "carga.h"

typedef enum {
    TEMPORIZADOR_ESP = 0,
    TEMPORIZADOR_FREERTOS = 1
} temporizador_origen_t;

typedef struct {
    bool valido;
    carga_modo_t modo;
    uint32_t periodo_ms;
    uint32_t activaciones;
    uint32_t intervalos;
    uint32_t rebases;
    uint32_t perdidos_total;

    double medio_ms;
    double minimo_ms;
    double maximo_ms;
    double jitter_ms;
} temporizadores_resumen_t;

/* Arranca las dos fuentes con el mismo periodo. */
esp_err_t temporizadores_iniciar(uint32_t periodo_ms);

/* Reconfigura ambas fuentes y reinicia las medidas. */
esp_err_t temporizadores_fijar_periodo(uint32_t periodo_ms);

esp_err_t temporizadores_obtener_periodo(
    uint32_t *periodo_ms
);

/* Consulta el ultimo resumen completo de un modo. */
esp_err_t temporizadores_ultimo_resumen(
    temporizador_origen_t origen,
    carga_modo_t modo,
    temporizadores_resumen_t *resumen
);

/* Total de envios fallidos a la cola. */
esp_err_t temporizadores_perdidos(
    temporizador_origen_t origen,
    uint32_t *total
);

#endif
