#pragma once

// Configura el ADC y calcula el voltaje cero en reposo
void initialize_sensor(void);

// Lee el ADC, calcula el voltaje y devuelve la presion en milibares
float read_pressure(void);