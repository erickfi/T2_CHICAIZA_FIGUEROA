idf_component_register(
    SRCS "consola.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_uart led pulsador bme280
)