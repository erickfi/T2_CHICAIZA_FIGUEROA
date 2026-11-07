idf_component_register(
    SRCS "pulsador.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_timer esp_driver_gpio
)