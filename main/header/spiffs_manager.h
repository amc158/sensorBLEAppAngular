#pragma once

// Inicializa el sistema de archivos SPIFFS en la memoria Flash
void initialize_spiffs(void);

// Guarda el valor proporcionado en formato JSON dentro de data.json
void save_reading_json(float value);

// Elimina el archivo data.json físico para liberar espacio
void clear_spiffs_memory(void);