#ifndef T1_CONSOLA_H
#define T1_CONSOLA_H

#include "esp_err.h"

/* Instala UART0 y arranca la tarea. 
* Se utiliza USB-a-UART, NO USB nativo. */
esp_err_t consola_iniciar(void);

#endif