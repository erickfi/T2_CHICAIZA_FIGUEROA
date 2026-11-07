#include "bme280.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "BME280";

/* Registros y codigos de identificacion: datasheet Bosch BME280, secc. 5. */
#define REG_CALIB_1        0x88
#define REG_CALIB_2        0xE1
#define REG_ID             0xD0
#define REG_CTRL_HUM       0xF2
#define REG_CTRL_MEAS      0xF4
#define REG_CONFIG         0xF5
#define REG_DATOS          0xF7
#define ID_BME280          0x60
#define ID_BMP280          0x58
#define I2C_PLAZO_MS       100
#define I2C_HZ             100000U

/* Calibracion de fabrica, PRIVATE al componente. */
typedef struct {
    uint16_t t1, p1;
    int16_t t2, t3;
    int16_t p2, p3, p4, p5, p6, p7, p8, p9;
    uint8_t h1, h3;
    int16_t h2, h4, h5;
    int8_t h6;
} calibracion_t;

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_dispositivo = NULL;
static SemaphoreHandle_t s_mutex = NULL;
static calibracion_t s_cal;
static bme280_estado_t s_estado;
static uint32_t s_periodo_ms = 0;

/* Convierte dos bytes little-endian a enteros de 16 bits. */
static uint16_t u16_le(const uint8_t *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

static int16_t i16_le(const uint8_t *bytes)
{
    uint16_t n = u16_le(bytes);
    return (int16_t)((n < 32768U) ? (int32_t)n : ((int32_t)n - 65536));
}

/* Lee registros consecutivos con escritura + repeated START + lectura. */
static esp_err_t leer_registros(uint8_t reg, uint8_t *destino, size_t cantidad)
{
    return i2c_master_transmit_receive(s_dispositivo, &reg, 1,
                                       destino, cantidad, I2C_PLAZO_MS);
}

static esp_err_t escribir_registro(uint8_t reg, uint8_t valor)
{
    uint8_t bytes[] = { reg, valor };
    return i2c_master_transmit(s_dispositivo, bytes, sizeof(bytes), I2C_PLAZO_MS);
}

static esp_err_t leer_calibracion(bool es_bme)
{
    uint8_t bloque_1[26]; /* 0x88 a 0xA1, inclusive. */
    esp_err_t err = leer_registros(REG_CALIB_1, bloque_1, sizeof(bloque_1));
    if (err != ESP_OK) {
        return err;
    }

    s_cal.t1 = u16_le(&bloque_1[0]);
    s_cal.t2 = i16_le(&bloque_1[2]);
    s_cal.t3 = i16_le(&bloque_1[4]);
    s_cal.p1 = u16_le(&bloque_1[6]);
    s_cal.p2 = i16_le(&bloque_1[8]);
    s_cal.p3 = i16_le(&bloque_1[10]);
    s_cal.p4 = i16_le(&bloque_1[12]);
    s_cal.p5 = i16_le(&bloque_1[14]);
    s_cal.p6 = i16_le(&bloque_1[16]);
    s_cal.p7 = i16_le(&bloque_1[18]);
    s_cal.p8 = i16_le(&bloque_1[20]);
    s_cal.p9 = i16_le(&bloque_1[22]);

    if (s_cal.t1 == 0 || s_cal.p1 == 0) {
        return ESP_ERR_INVALID_RESPONSE; /* Coeficientes no plausibles. */
    }

    if (!es_bme) {
        return ESP_OK; /* BMP280: sin coeficientes de humedad. */
    }

    s_cal.h1 = bloque_1[25]; /* Registro 0xA1. */
    uint8_t bloque_2[7];    /* Registros 0xE1 ... 0xE7. */
    err = leer_registros(REG_CALIB_2, bloque_2, sizeof(bloque_2));
    if (err != ESP_OK) {
        return err;
    }

    s_cal.h2 = i16_le(&bloque_2[0]);
    s_cal.h3 = bloque_2[2];
    /* H4 y H5 son coeficientes CON SIGNO de 12 bits repartidos. */
    uint16_t h4 = (uint16_t)(((uint16_t)bloque_2[3] << 4) |
                             (bloque_2[4] & 0x0FU));
    uint16_t h5 = (uint16_t)(((uint16_t)bloque_2[5] << 4) |
                             (bloque_2[4] >> 4));
    s_cal.h4 = (int16_t)((h4 & 0x800U) ? ((int32_t)h4 - 4096) : h4);
    s_cal.h5 = (int16_t)((h5 & 0x800U) ? ((int32_t)h5 - 4096) : h5);
    s_cal.h6 = (int8_t)((bloque_2[6] <= 127) ? bloque_2[6] :
                         ((int32_t)bloque_2[6] - 256));
    return ESP_OK;
}

/* Formulas del apendice 8.1 de Bosch BME280 (double precision).
 * Se ejecutan 1 vez/s: no hay calculos de coma flotante en la ISR.
 * t_fine debe calcularse con temperatura ANTES que presion y humedad.
 */
static bool compensar(int32_t adc_t, int32_t adc_p, int32_t adc_h,
                      bool es_bme, bme280_estado_t *muestra)
{
    double v1 = ((double)adc_t / 16384.0 - s_cal.t1 / 1024.0) * s_cal.t2;
    double tbase = (double)adc_t / 131072.0 - s_cal.t1 / 8192.0;
    double v2 = tbase * tbase * s_cal.t3;
    int32_t t_fine = (int32_t)(v1 + v2);
    double temperatura = (v1 + v2) / 5120.0;

    v1 = t_fine / 2.0 - 64000.0;
    v2 = v1 * v1 * s_cal.p6 / 32768.0;
    v2 += v1 * s_cal.p5 * 2.0;
    v2 = v2 / 4.0 + s_cal.p4 * 65536.0;
    v1 = (s_cal.p3 * v1 * v1 / 524288.0 + s_cal.p2 * v1) / 524288.0;
    v1 = (1.0 + v1 / 32768.0) * s_cal.p1;
    if (v1 == 0.0) {
        return false; /* Evita dividir por cero. */
    }
    double presion = 1048576.0 - (double)adc_p;
    presion = (presion - v2 / 4096.0) * 6250.0 / v1;
    v1 = s_cal.p9 * presion * presion / 2147483648.0;
    v2 = presion * s_cal.p8 / 32768.0;
    presion += (v1 + v2 + s_cal.p7) / 16.0; /* Resultado en Pa. */

    double humedad = 0.0;
    if (es_bme) {
        double vh = (double)t_fine - 76800.0;
        vh = (adc_h - (s_cal.h4 * 64.0 + s_cal.h5 / 16384.0 * vh)) *
             (s_cal.h2 / 65536.0 * (1.0 + s_cal.h6 / 67108864.0 * vh *
             (1.0 + s_cal.h3 / 67108864.0 * vh)));
        vh = vh * (1.0 - s_cal.h1 * vh / 524288.0);
        if (vh > 100.0) vh = 100.0;
        if (vh < 0.0) vh = 0.0;
        humedad = vh;
    }

    /* Rango de funcionamiento especificado por Bosch, no 'calibracion extra'.
     * Los valores NaN tambien fallan estas comparaciones. */
    if (!(temperatura >= -40.0 && temperatura <= 85.0 &&
          presion >= 30000.0 && presion <= 110000.0 &&
          (!es_bme || (humedad >= 0.0 && humedad <= 100.0)))) {
        return false;
    }
    muestra->temperatura_c = (float)temperatura;
    muestra->presion_hpa = (float)(presion / 100.0); /* Pa a hPa. */
    muestra->humedad_pct = (float)humedad;
    muestra->tiene_humedad = es_bme;
    muestra->instante_us = esp_timer_get_time();
    muestra->muestra_valida = true;
    return true;
}

static esp_err_t leer_muestra(bme280_estado_t *muestra)
{
    bool es_bme = (s_estado.id == ID_BME280);
    uint8_t bytes[8] = {0};
    esp_err_t err = leer_registros(REG_DATOS, bytes, es_bme ? 8U : 6U);
    if (err != ESP_OK) {
        return err;
    }
    int32_t adc_p = (int32_t)(((uint32_t)bytes[0] << 12) |
                              ((uint32_t)bytes[1] << 4) | (bytes[2] >> 4));
    int32_t adc_t = (int32_t)(((uint32_t)bytes[3] << 12) |
                              ((uint32_t)bytes[4] << 4) | (bytes[5] >> 4));
    int32_t adc_h = (int32_t)(((uint32_t)bytes[6] << 8) | bytes[7]);
    if (adc_p == 0x80000 || adc_t == 0x80000) {
        return ESP_ERR_INVALID_RESPONSE; /* Medicion omitida o no disponible. */
    }
    return compensar(adc_t, adc_p, adc_h, es_bme, muestra) ?
           ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static void tarea_bme280(void *argumento)
{
    (void)argumento;
    /* Tras entrar en modo normal se espera a la primera conversion. */
    vTaskDelay(pdMS_TO_TICKS(25));
    TickType_t proximo = xTaskGetTickCount();
    const TickType_t periodo = pdMS_TO_TICKS(s_periodo_ms);

    for (;;) {
        bme280_estado_t lectura = {0};
        esp_err_t err = leer_muestra(&lectura);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        if (err == ESP_OK) {
            s_estado.temperatura_c = lectura.temperatura_c;
            s_estado.presion_hpa = lectura.presion_hpa;
            s_estado.humedad_pct = lectura.humedad_pct;
            s_estado.instante_us = lectura.instante_us;
            s_estado.muestra_valida = true;
        } else {
            s_estado.muestra_valida = false;
            ++s_estado.errores_lectura;
        }
        xSemaphoreGive(s_mutex);

        if (err == ESP_OK) {
            if (s_estado.id == ID_BME280) {
                ESP_LOGI(TAG, "T=%.2f C, P=%.2f hPa, HR=%.2f %%; t=%lld us",
                         (double)lectura.temperatura_c, (double)lectura.presion_hpa,
                         (double)lectura.humedad_pct, (long long)lectura.instante_us);
            } else {
                ESP_LOGI(TAG, "BMP280: T=%.2f C, P=%.2f hPa; t=%lld us",
                         (double)lectura.temperatura_c, (double)lectura.presion_hpa,
                         (long long)lectura.instante_us);
            }
        } else {
            ESP_LOGW(TAG, "Muestra no valida: %s", esp_err_to_name(err));
        }
        xTaskDelayUntil(&proximo, periodo);
    }
}

esp_err_t bme280_iniciar(const bme280_config_t *config)
{
    if (config == NULL || s_bus != NULL ||
        (int)config->sda < 0 || (int)config->scl < 0 ||
        config->sda == config->scl ||
        (config->direccion != 0x76U && config->direccion != 0x77U) ||
        config->periodo_ms < 100U ||
        pdMS_TO_TICKS(config->periodo_ms) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = config->sda,
        .scl_io_num = config->scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = false, /* Pull-up externos en modulo. */
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        return err;
    }

    /* Detectar PRIMERO la direccion fisica indicada en pines.h. */
    err = i2c_master_probe(s_bus, config->direccion, I2C_PLAZO_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No responde direccion 0x%02X: %s",
                 config->direccion, esp_err_to_name(err));
        goto fallo;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = config->direccion,
        .scl_speed_hz = I2C_HZ,
    };
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dispositivo);
    if (err != ESP_OK) {
        goto fallo;
    }

    uint8_t id = 0;
    err = leer_registros(REG_ID, &id, 1);
    if (err != ESP_OK) {
        goto fallo;
    }
    ESP_LOGI(TAG, "Dispositivo en 0x%02X, ID=0x%02X", config->direccion, id);
    if (id != ID_BME280 && id != ID_BMP280) {
        ESP_LOGE(TAG, "ID desconocido (BME280=0x60; BMP280=0x58)");
        err = ESP_ERR_NOT_SUPPORTED;
        goto fallo;
    }
    if (id == ID_BMP280) {
        ESP_LOGW(TAG, "BMP280 detectado: NO proporciona humedad");
    }

    err = leer_calibracion(id == ID_BME280);
    if (err != ESP_OK) {
        goto fallo;
    }

    /* Bosch: configurar en reposo; ctrl_hum antes de ctrl_meas.
     * 0x20: standby 62.5 ms, filtro desactivado, SPI de 4 hilos inactivo.
     * 0x01: humedad x1. 0x27: T x1, P x1, modo normal.
     */
    err = escribir_registro(REG_CONFIG, 0x20);
    if (err != ESP_OK) goto fallo;
    if (id == ID_BME280) {
        err = escribir_registro(REG_CTRL_HUM, 0x01);
        if (err != ESP_OK) goto fallo;
    }
    err = escribir_registro(REG_CTRL_MEAS, 0x27);
    if (err != ESP_OK) goto fallo;

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        err = ESP_ERR_NO_MEM;
        goto fallo;
    }
    s_periodo_ms = config->periodo_ms;
    memset(&s_estado, 0, sizeof(s_estado));
    s_estado.id = id;
    s_estado.direccion = config->direccion;
    s_estado.tiene_humedad = (id == ID_BME280);

    if (xTaskCreate(tarea_bme280, "tarea_bme280", 4096, NULL, 3, NULL) != pdPASS) {
        err = ESP_ERR_NO_MEM;
        goto fallo;
    }
    ESP_LOGI(TAG, "Muestreo cada %lu ms; bus a %u Hz",
             (unsigned long)s_periodo_ms, (unsigned)I2C_HZ);
    return ESP_OK;

fallo:
    if (s_mutex != NULL) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }
    if (s_dispositivo != NULL) {
        i2c_master_bus_rm_device(s_dispositivo);
        s_dispositivo = NULL;
    }
    i2c_del_master_bus(s_bus);
    s_bus = NULL;
    return err;
}

esp_err_t bme280_obtener_estado(bme280_estado_t *estado)
{
    if (estado == NULL) return ESP_ERR_INVALID_ARG;
    if (s_mutex == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return ESP_FAIL;
    *estado = s_estado;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}