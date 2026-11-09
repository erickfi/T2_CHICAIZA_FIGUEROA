
idf_component_register(
    SRCS "periodica.c"
    INCLUDE_DIRS "include"
    REQUIRES carga
    PRIV_REQUIRES bme280 esp_timer
)
