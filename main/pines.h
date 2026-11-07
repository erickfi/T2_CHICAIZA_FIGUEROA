#pragma once

#include "driver/gpio.h"

/* UNICO LUGAR DEL PROYECTO PARA ASIGNAR GPIO. */
#define PIN_LED                  GPIO_NUM_4
#define PIN_PULSADOR             GPIO_NUM_5

/* 
* Niveles electricos configurables: 0 o 1
* estos van a depender de la conexión de los elementos.
* Configurados en este punto por conexión pull-up
*/

#define LED_NIVEL_ENCENDIDO      1
#define PULSADOR_NIVEL_ACTIVO    1

/* periodo 1000 ms */
#define LED_PERIODO_MS           1000U

/* medir rebote real */
#define PULSADOR_ANTIRREBOTE_MS  20U

/* Pines para conexiones de sensor BME280. */
#define PIN_BME280_SDA           GPIO_NUM_8
#define PIN_BME280_SCL           GPIO_NUM_9

/* BME280: direccion I2C de 7 bits. */
#define BME280_DIRECCION  0x77U
/* Si el sensor responde en 0x77, cambiar 0x77U */

/* Periodo de lectura y publicacion. */
#define BME280_PERIODO_MS 1000U
