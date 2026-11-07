
#ifndef T1_BME280_H
#define T1_BME280_H

#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "esp_err.h"

/* Configuracion que recibe el componente. */
typedef struct {
    gpio_num_t sda;
    gpio_num_t scl;
    uint8_t direccion;
    uint32_t periodo_ms;
} bme280_config_t;

/* Ultima medida y estado del sensor. */
typedef struct {
    uint8_t id;
    uint8_t direccion;
    bool muestra_valida;
    bool tiene_humedad;

    float temperatura_c;
    float presion_hpa;
    float humedad_pct;

    int64_t instante_us;
    uint32_t errores_lectura;
} bme280_estado_t;

esp_err_t bme280_iniciar(
    const bme280_config_t *config
);

esp_err_t bme280_obtener_estado(
    bme280_estado_t *estado
);

#endif
