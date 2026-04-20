#pragma once
#include <stdbool.h>

// Variables globales accesibles desde el main mediante 'extern'
extern bool is_sensor_active;
extern bool is_spiffs_dump_requested;
extern bool is_spiffs_clear_requested;

// Arranca el Bluetooth LE y empieza a emitir (Advertising)
void initialize_bluetooth(void);

// Envia un valor en coma flotante por notificacion GATT
void send_ble_data(float value);

// Envia -9999.0 para avisar a Angular que se ha terminado de leer la memoria
void send_ble_end_marker(void);