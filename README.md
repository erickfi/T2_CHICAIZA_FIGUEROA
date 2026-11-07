# TALLER 1 - Periféricos: GPIO, I2C y UART

**Proyecto:** T1_CHICAIZA_FIGUEROA

**Curso:** 2026-27

**Sistemas Empotrados y de Tiempo Real**

## Integrantes y roles
- Chicaiza Freddy - Programación y montaje
- Figueroa Erick - Programación y documentación

## Estructura del proyecto 

```text
T1_CHICAIZA_FIGUEROA/
|-- CMakeLists.txt
|-- sdkconfig.defaults
|-- .gitignore
|-- README.md
|-- images
|-- main/
|   |-- CMakeLists.txt
|   |-- main.c
|   `-- pines.h
`-- components/
    |-- led/
    |   |-- CMakeLists.txt
    |   |-- led.c
    |   `-- include/led.h
    |-- pulsador/
    |   |-- CMakeLists.txt
    |   |-- pulsador.c
    |   `-- include/pulsador.h
    `-- consola/
    |   |-- CMakeLists.txt
    |   |-- consola.c
    |   `-- include/consola.h
    `-- bme280/
        ├── CMakeLists.txt
        ├── bme280.c
        └── include/bme280.h
```

- `main/pines.h` es el **unico lugar de asignacion** de GPIO y polaridad electrica.
- `main/main.c` crea las configuraciones y llama a las inicializaciones. No controla pines en el bucle.
- `led` gestiona su estado y temporizacion; no incluye `pines.h`.
- `pulsador` recibe flancos mediante ISR y cola y confirma el nivel desde una tarea.
- `consola` despacha una tabla de comandos y llama a interfaces publicas de LED/pulsador.
- `esp_driver_gpio` / `esp_driver_uart` son controladores suministrados por ESP-IDF; `esp_timer` aporta marcas de tiempo.

**Componente:** modulo con su `.c`, su API publica en `include/*.h` y su declaracion `CMakeLists.txt`.
**API publica:** funciones y tipos a los que puede acceder otro componente, sin acceder a su estado privado.
**Estado interno:** variables que solo puede modificar el propio componente.

## Hardware, alimentacion y mapa de pines

- **Placa utilizada:** ESP32-S3-WROOM-1

|**Señal**| **Práctica** | **GPIO** | **Consideraciones** |
|---| --- | --- | --- |
|LED| P1A | 4 | Conexión en forma PULLUP, en `components/led/led.c` se puede cambiar <br> la configuración para usar las resistencia internas.|
|Pulsador| P1B | 5 | Se utiliza la conexión PULLUP, en caso de utilizar otra configuración <br> cambiar el estado activo en `main/pines.h`  |
|BME280 - I2C SDA| P1C | 8  |  |
|BME280 - I2C SCL| P1C | 9  |  |
|UART0 | P2 | | Configuración 115200, 8N1. Utilizar conector USB-UART.


**Consideraciones** 
- En el archivo `main/pines.h` se encuentran consideraciones adicionales de configuración a tener en cuenta al momento de replicar este proyecto, una de esas consideraciones la dirección I2C y frecuencias de antirrebotes.
- Al replicar el proyecto confirmar los pines, considerar que se trabaja a 3.3V.
- `PULSADOR_ANTIRREBOTE_MS`: valor **provisional de 20 ms**. Al replicar debe ajustarse midiendo el rebote real.
- Al cambiar el número de los pines asegurarse de no utilizar aquellos reservados para la memoria o el arranque, verificar el en esquema de pinout de la placa utilizada.

## Desarrollo

### P1A · Salida digital y parpadeo estable

Al arrancar, la función `tarea_led` conmuta la salida con espera `xTaskDelayUntil()` cada 500 ms.
Dicha funcion espera hasta instantes periodicos usando el tick como base, evitando sumar al periodo el tiempo normal de procesamiento del bucle. Se registra una marca temporal en microsegundos con `esp_timer_get_time()` inmediatamente despues del cambio de GPIO.

Para calcular el periodo entre dos encendidos consecutivos, se restan los valores `t` de dos lineas `ON` seguidas; debe aproximarse a 1 000 000 us o 500 000 us entre conmutaciones sucesivas dependiendo del tiempo probado.
El periodo observado puede mostrar un jitter por planificación y trazas.

`led_fijar_modo()` recibe uno de tres modos: `LED_MODO_APAGADO`, `LED_MODO_ENCENDIDO`, `LED_MODO_PARPADEO`.
En modo fijo, el pin cambia inmediatamente desde la función que da la orden; en modo parpadeo vuelve a conmutarse desde la tarea periodica.

El primer encendido tras `led blink` puede demorarse hasta 500 ms, porque se mantiene la referencia periodica interna, finalmente, un **mutex** protege el modo y el nivel lógico frente a accesos simultaneos de la tarea LED y la consola. El registro de trazas se hace fuera del mutex.

### P1B · Pulsador con interrupción y antirrebote

El pin utiliza una resistencia de polarizacion externa pull-up. Las internas se dejan desactivadas, sin embargo, de ser requerido se pueden habilitar.
Se configura interrupcion en ambos flancos para distinguir pulsacion y liberacion, de esta forma se evita falsos positivos.

- `ISR:` toma la marca de tiempo y la coloca en una cola de 20 elementos (`xQueueSendFromISR`). No imprime ni espera. Si la cola esta llena, incrementa `flancos_perdidos`.
- La función recibe el instante, descarta flancos dentro de la ventana de guarda, espera 20 ms y comprueba el nivel electrico definitivo. Solo incrementa el contador en la transicion validada de reposo a pulsado. La liberacion confirmada vuelve a dejar el pulsador preparado.
- API publica: `pulsador_obtener_estado()` entrega una copia coherente de los contadores, que puede utilizar la consola sin tener acceso directo a las variables privadas.
- `portMUX_TYPE` protege exclusivamente la lectura/escritura de los dos contadores compartidos; las secciones criticas son breves. El tratamiento de rebote permanece fuera de la ISR.
- Limite: una pulsacion fisica mas breve que la ventana de guarda podria no superar la confirmacion del nivel. Por ello medir el rebote de este pulsador y elegir un valor que evite falsos eventos sin impedir pulsaciones cortas normales es parte fundamental para garantizar su correcto funcionamiento.

### P1C · Lectura del sensor BME280 por I2C

- Estructura: componente components/bme280/, API pública bme280_iniciar() y bme280_obtener_estado(). El único fichero que asigna los GPIO SDA/SCL, dirección y periodo es `main/pines.h`.
- Bus: controlador maestro `driver/i2c_master.h`, I2C0 a 100 kHz, resistencias de pull-up internas desactivadas porque la placa del sensor debe incorporarlas. No conectar a 5 V.
- Diagnóstico antes de medir: se comprueba respuesta mediante i2c_master_probe(), se lee 0xD0 y se exige ID 0x60 (BME280) o se acepta ID 0x58 (BMP280, solo temperatura y presión). Otros IDs se rechazan.
- Calibración: se leen los parámetros de compensación Bosch 0x88..0xA1, y 0xE1..0xE7 solo en BME280. Se utilizan las fórmulas de doble precisión del apéndice 8.1 de la hoja de datos Bosch; se muestran unidades °C, hPa y %HR. No se presentan registros ADC sin compensar como medidas.
- Configuración: modo normal, sobremuestreo ×1 para temperatura, presión y humedad (esta última solo BME280); sin filtro IIR, standby de 62,5 ms. El firmware publica una muestra cada 1000 ms mediante tarea FreeRTOS con xTaskDelayUntil().
- Integración: la tarea del sensor es la única que accede al bus. estado lee una copia de la última muestra con un mutex, sin bloquearse en una transferencia I2C. Una inicialización fallida no reinicia el firmware: LED, pulsador y consola siguen activos. Si falla una lectura periódica, se marca como no válida y se incrementa el contador de errores.

### P2 · Consola serie por UART

Se instala el driver de UART0 con buffers de 2048 bytes; linea `115200, 8N1, sin control de flujo`. `uart_set_pin()` recibe `UART_PIN_NO_CHANGE` para no cambiar la conexion fisica que ya utiliza el USB-a-UART. `uart_vfs_dev_use_driver()` integra los mensajes enviados por stdout/ESP_LOG con el driver.

La función `tarea_consola` recibe caracteres hasta `\r` (Intro en el monitor), `\n` o `\r\n`.
Acepta hasta **96 caracteres utiles**. Si llega uno mas, descarta **toda la linea** hasta el siguiente fin de linea, avisa del error y vuelve a mostrar el prompt. Una linea vacia solo muestra de nuevo el prompt. Se limita tambien la cantidad de argumentos.

| Comando | Efecto |
|---|---|
| `ayuda` | Recorre la tabla de comandos e imprime su sintaxis |
| `estado` | Muestra modo/nivel funcional del LED, periodo, pulsaciones y flancos perdidos. Aclara que BME280 esta pendiente |
| `led on` | Enciende el LED en modo fijo |
| `led off` | Apaga el LED en modo fijo |
| `led blink` | Recupera el parpadeo periodico |

`led on`, `led off` y `led blink` son tres variantes del mismo comando `led`; existen tres comandos principales: `ayuda`, `estado` y `led`. La recepcion, el analisis y la ejecucion no se hacen en `main.c`.

Los mensajes de registro del LED o del pulsador pueden aparecer entre el prompt y la respuesta, porque el mismo puerto transporta ambas cosas. No es una segunda consola ni un defecto del UART. El monitor no implementa aqui edicion avanzada de linea ni historico de comandos.

## Revisiones, observaciones y correcciones

El presente apartado resume de manera general los cambios realizados desde la última revisión.

| Hallazgo de revision | Correccion efectuada | Comprobacion actual |
|---|---|---|
| GPIO repetidos en implementaciones | Solo `main/pines.h` los asigna; structs de configuracion | Busqueda textual y revision de fuentes |
| Polaridad LED fijada a 1 | Parametro `nivel_encendido` 0/1; apagado por complemento | Revision de ambas ramas |
| Ausencia P1A | Tarea LED con periodo absoluto, trazas temporales | Codigo revisado; medicion real pendiente |
| Pulso contado tambien al soltar | Detectar ambos flancos, confirmar transicion estable y estado anterior | Revision del algoritmo; pruebas fisicas pendientes |
| Carrera de lectura/escritura del contador | Seccion critica breve en acceso a contadores | Revision de entradas/salidas del bloqueo |
| Flanco perdido sin diagnostico | Contador de cola llena en ISR | API y comando `estado` lo exponen |
| Linea larga ejecutada truncada | Bandera `desbordada` hasta CR/LF y posterior descarte | Simulacion automatizada local: superada |
| CRLF procesado dos veces | Ignorar LF inmediatamente despues de CR | Simulacion automatizada local: superada |
| Compilacion con avisos por argumentos sin uso | `(void)argumento` y `(void)argv` | GCC `-Wall -Wextra -Wpedantic -Werror` con stubs: sin avisos |
| Dependencias incorrectas o mayusculas de include | CMake corregido | Revision estructural; CMake ESP-IDF real pendiente |
| GPIO interrupt podia no quedar activado explicitamente | `gpio_intr_enable()` despues de instalar ISR | Revision del orden de llamadas |
| `sdkconfig` anterior imponia 100 Hz | `sdkconfig.defaults` a 1000 Hz y procedimiento de regeneracion | Comprobar en SDK Editor y monitor en placa |

## Pruebas y evidencias

| Prueba | Resultado esperado | 
|---|---|
| Sensor desconectado | Mensaje de error; LED y consola siguen respondiendo |
| Detección I2C | Dirección configurada responde (`0x76` / `0x77`) | 
| Identificación | `0x60` BME280 / `0x58` BMP280 | 
| Lectura en reposo | Temperatura, presión y humedad plausibles y relativamente estables |
| Acercar la mano | Temperatura cambia de modo coherente | 
| `estado` | Misma última lectura validada sin bloquear la consola | 
| Unir todas las tareas | LED periódico, pulsador y consola funcionando a la vez | 


![Pruebas de consola](images/consola0.jpeg)
![Pruebas de consola](images/consola1.jpeg)
![Pruebas de consola](images/consola2.jpeg)
![Montaje de componentes](images/montaje.jpeg)

## Referencias tecnicas

- `T1_1_Guion_del_taller_V01-04`
- `T1_3_Apoyo_alumno_V01-05`
- `T1_0_Sesion_de_preparacion_V01-04.pdf`
- https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/
- https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-guides/build-system.html
- https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-reference/peripherals/uart.html
- https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-reference/peripherals/gpio.html
- https://www.freertos.org/Documentation/00-Overview