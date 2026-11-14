# TALLER 2 - tareas, prioridades y temporización

**Proyecto:** T2_CHICAIZA_FIGUEROA

**Curso:** 2026-27

**Sistemas Empotrados y de Tiempo Real**

## Integrantes y roles
- Chicaiza Freddy - Programación y montaje
- Figueroa Erick - Programación y documentación

## Listado de pines

| **Elemento**	| **GPIO configurado** |	**Observación** |
|---|---|---|
| LED externo	| GPIO4 |	Salida; P3 utilizará LEDC |
| Pulsador |	GPIO5 |	Entrada con resistencia externa |
| BME280 SDA |	GPIO8 |	Datos I2C |
| BME280 SCL |	GPIO9 |	Reloj I2C |
| BME280 |	Dirección 0x77 |	Debe confirmarse mediante lectura



# Consideraciones


### Arquitectura e integración

El Taller 2 amplía los componentes reutilizados del Taller 1 e incorpora tres prácticas funcionales: P1 (tareas y prioridades), P2 (adquisición periódica del sensor) y P3 (control del LED mediante PWM hardware).

Se mantienen los componentes `led`, `pulsador`, `bme280` y `consola`, y se incorporan dos componentes:

- **`carga`:** crea dos tareas de cálculo fijadas al núcleo 0 y una tarea de informe fijada al núcleo 1. Permite cambiar entre parada, turnos, inanición y corregida.
- **`periodica`:** crea la tarea de adquisición del BME280, utiliza espera absoluta y calcula estadísticas de temporización separadas por modo de carga.

El control de LEDC permanece dentro del componente `led`. El pulsador notifica las pulsaciones después del antirrebote, sin ejecutar operaciones LEDC dentro de la interrupción.

Se modifica el componente BME280 para que no cree una tarea propia de adquisición. En su lugar, P2 realiza las lecturas a través de `bme280_leer()`. De este modo, la activación periódica y la adquisición quedan bajo el control de una única tarea.

La consola UART0 se mantiene como interfaz común para las tres prácticas.

### Tabla de tareas y prioridades

| Tarea | Prioridad | Núcleo | Activación | Pila asignada |
|---|---|---|---|---|
| carga1 | 2 / 3 | CPU0 | 20 ms cálculo + 27 ms espera; sin bloqueo en inanición | 3072 bytes |
| carga2 | 2 | CPU0 | 20 ms cálculo + 27 ms espera | 3072 bytes |
| informe | 2 | CPU1 | Cada 1000 ms | 3072 bytes |
| periodica | 3 | CPU0 | Cada 1000 ms | 4096 bytes |
| tarea_ledc | 5 | Sin afinidad fija | Orden recibida por cola | 4096 bytes |
| tarea_consola | 4 | Sin afinidad fija | Recepción UART | 4096 bytes |
| tarea_pulsador | 10 | Sin afinidad fija | Evento GPIO | 3072 bytes |

Las prioridades indicadas son las configuradas en el código. Las marcas de agua de pila mínima libre están pendientes de medición con `uxTaskGetStackHighWaterMark()`.

### Justificación de prioridades

**Tareas de carga:** utilizan prioridad 2 en modo turnos. En inanición, `carga1` aumenta a prioridad 3 y deja de bloquearse, impidiendo que avance `carga2`. En modo corregida conserva la prioridad superior, pero recupera su espera bloqueada. Así se demuestra la diferencia entre prioridad e inanición.

**Tarea periódica:** utiliza prioridad 3 y se fija al núcleo 0 para compartir procesador con las tareas de carga. En modo turnos tiene mayor prioridad que la carga (2), con el objetivo de proteger la regularidad de sus activaciones. La adecuación temporal se comprobará mediante las medidas de jitter y rebases.

**Tarea de informe:** utiliza prioridad 2 y está fijada al núcleo 1 para que pueda informar de los contadores incluso cuando la carga acapare el núcleo 0.

**Tarea LEDC:** utiliza prioridad 5 para procesar las órdenes de control del LED. La generación PWM y las transiciones de brillo se realizan por hardware, sin requerir que la tarea actualice continuamente el duty.

**Tarea de consola:** utiliza prioridad 4, superior a las tareas de carga. Esto permite atender los comandos de cambio de modo durante las pruebas de inanición.

**Tarea del pulsador:** conserva la prioridad 10 del componente existente. Procesa los avisos de interrupción y confirma los cambios de estado mediante antirrebote antes de notificar a la aplicación.

Los tamaños de pila son valores de configuración iniciales y deberán validarse mediante las marcas de agua medidas.

### Frecuencias y periodos configurados

| Elemento | Configuración |
|---|---|
| Tick de FreeRTOS | 1000 Hz (1 ms por tick) |
| Ráfaga de cálculo P1 | 20 ms |
| Espera bloqueada P1 | 27 ms |
| Ciclo nominal de carga | 47 ms |
| Informe P1 | 1000 ms |
| Muestreo BME280 | 1000 ms |
| Bus I2C | 100 kHz |
| UART0 | 115200 baudios, 8N1 |
| PWM LEDC | 5000 Hz, 13 bits |
| Transición LEDC | 1500 ms solicitados, precedidos por 400 ms de apagado |

### P1 — Tareas y prioridades

Se crean dos tareas de carga en CPU0. En modo turnos, ambas tienen prioridad 2 y alternan una ráfaga de cálculo con una espera bloqueada.

En modo inanición, la primera tarea aumenta a prioridad 3 y deja de bloquearse, por lo que la segunda deja de avanzar. Se espera observar el aviso del Task Watchdog correspondiente a la tarea inactiva del núcleo 0.

En modo corregida, la primera tarea conserva su prioridad 3, pero vuelve a bloquearse después de cada ráfaga. Esto permite que ambas tareas progresen.

El ciclo de carga nominal de 47 ms se selecciona para evitar que las activaciones del sensor, con periodo de 1000 ms, coincidan siempre con el mismo punto de ese ciclo.

**Resultados:** pendientes de adjuntar las trazas de turnos, inanición y corrección, incluida la identificación de las tareas mostradas por el watchdog.

### P2 — Tarea periódica y medición temporal

Se crea una tarea periódica fijada a CPU0 que realiza una lectura del BME280 cada 1000 ms mediante `xTaskDelayUntil()`.

La marca temporal se obtiene con `esp_timer_get_time()` inmediatamente después de la espera absoluta y antes de acceder al sensor, evitando incorporar la duración variable de la transacción I2C a la medida de activación.

Se utiliza una ventana de 50 activaciones, que proporciona 49 intervalos consecutivos. Para cada ventana se calculan el intervalo medio, mínimo, máximo, jitter y número de rebases del periodo.

Las estadísticas se separan por modo de carga. Cuando cambia el modo, se descarta la ventana incompleta y comienza una nueva. Se conserva el último resumen completo de cada modo para su consulta desde la consola.

**Periodo de muestreo:** se conserva inicialmente 1000 ms porque el objetivo es monitorizar variables ambientales cuya evolución normal es lenta respecto a ese intervalo. La elección deberá contrastarse con los requisitos de la aplicación y las pruebas del sensor.

**Resultados pendientes:** registrar al menos un resumen completo en `PARADA` y otro en `TURNOS`, comparar ambos y justificar las diferencias observadas.

### P3 — Salida modulada con LEDC

El componente LED se adapta para utilizar LEDC en modo `LEDC_LOW_SPEED_MODE`, con 5000 Hz y 13 bits de resolución.

La frecuencia se elige para evitar parpadeo visual perceptible en condiciones normales. La resolución ofrece 8192 pasos por periodo PWM y cumple el límite de reloj indicado en el guion: 5000 × 8192 = 40,96 MHz, inferior a 80 MHz.

Los cuatro modos son apagado, brillo intermedio, encendido y transición hardware. Para hacer visible la transición, se establece inicialmente duty cero, se mantienen 400 ms de apagado y después se inicia el aumento progresivo de brillo.

Solo la tarea propietaria de LEDC modifica el duty o inicia transiciones. La consola y el pulsador envían órdenes a dicha tarea.

### P4 - Temporizadores

En esta práctica se comparan los temporizadores esp_timer y los temporizadores software de FreeRTOS, utilizando el mismo periodo de aproximadamente 10 ms.

Ambos mecanismos registrarán sus instantes de activación mediante esp_timer_get_time() y enviarán los datos a una cola. Una tarea en CPU1 calculará el periodo medio, mínimo y máximo, así como el jitter, utilizando ventanas de 50 activaciones.

Se configurará la tarea de servicio de temporizadores de FreeRTOS en CPU0 para que comparta núcleo con las tareas de carga. Además, se ampliará la consola para modificar el periodo de los temporizadores, consultar sus estadísticas y controlar el LED.

Las pruebas se realizarán en modo parada y en modo turnos, comparando los resultados obtenidos. Se espera que el temporizador software presente mayor jitter bajo carga, debido a que depende de una tarea de servicio de menor prioridad, mientras que esp_timer utiliza un servicio de mayor prioridad.

### Comandos de consola

| Comando | Función |
|---|---|
| `ayuda` | Lista los comandos |
| `estado` | Consulta LED, pulsador y BME280 |
| `led off` | Apaga el LED |
| `led medio` | Establece brillo intermedio |
| `led on` | Establece brillo máximo |
| `led fade` | Inicia transición por hardware |
| `led next` | Avanza al siguiente modo |
| `carga stop` | Detiene el cálculo de las tareas de carga |
| `carga turnos` | Activa las dos tareas con igual prioridad |
| `carga inanicion` | Activa la demostración de inanición |
| `carga corregida` | Recupera el bloqueo de la tarea prioritaria |
| `periodica` | Consulta el último resumen completo de cada modo |

### Verificaciones pendientes

Para finalizar la integración será necesario confirmar la compilación limpia, ejecutar las pruebas de P1 y P2 con P3 funcionando, registrar las medidas de jitter y rebases, comprobar el watchdog y medir la pila mínima libre de cada tarea.

<img width="1199" height="1312" alt="imagen" src="https://github.com/user-attachments/assets/108f631f-edd3-4a68-9d2b-03b22b882b16" />



### Referencias y asistencia

- `T2_1_Guion_del_taller_V01-02.pdf`, apartados 6.1, 6.2, 6.3 y 6.5.
- `T2_3_Apoyo_alumno_V01-01.pdf`.
- Material docente U3-S3, U3-S4, U3-S5 y U3-S6.
- Documentación oficial de ESP-IDF para ESP32-S3: https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/
- Documentación oficial de FreeRTOS y ESP-IDF para tareas, prioridades, temporización y supervisión de pila.
- Asistencia mediante ChatGPT (OpenAI) para revisión de código, análisis de compatibilidad entre componentes, propuestas de integración y redacción técnica. Las pruebas y validaciones finales corresponden a los autores del proyecto.
