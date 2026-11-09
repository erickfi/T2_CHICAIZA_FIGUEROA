idf_component_register(
    SRCS "temporizadores.c"
    INCLUDE_DIRS "include"
    REQUIRES carga esp_timer freertos
)
