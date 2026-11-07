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