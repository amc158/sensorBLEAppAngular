#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "cJSON.h"

// Archivos de Cabecera (Librerías Modulares)
#include "spiffs_manager.h"
#include "pressure_sensor.h"
#include "bluetooth_manager.h"

static const char *TAG = "MAIN_APP";

void app_main(void) {
    // Inicializar memoria base (Requerido por Bluetooth y WiFi)
    nvs_flash_init(); 

    // 1. Inicializacion de todos los componentes de hardware e interfaces
    initialize_spiffs();
    initialize_sensor();
    initialize_bluetooth();

    // Variables locales para gestionar la recoleccion de datos historicos
    int reading_count = 0;
    float pressure_sum = 0.0;

    // 2. Bucle infinito del programa
    while (1) {
        
        // --- SECCION 1: GESTION DE BORRADO DE MEMORIA ---
        if (is_spiffs_clear_requested) {
            clear_spiffs_memory();
            is_spiffs_clear_requested = false;
        }

        // --- SECCION 2: GESTION DE DESCARGA HACIA LA APP ---
        if (is_spiffs_dump_requested) {
            is_sensor_active = false;          // Pausar lectura en vivo
            reading_count = 0;                 // Limpiar sumatorios a medias
            pressure_sum = 0.0;
            
            // Abrir archivo en modo lectura
            FILE* f = fopen("/spiffs/data.json", "r");
            if (f != NULL) {
                char line[64];
                ESP_LOGI(TAG, "🚀 Iniciando proceso de descarga...");
                
                // Leer el archivo de texto linea por linea
                while (fgets(line, sizeof(line), f)) {
                    // Convertir el texto de la linea a formato JSON
                    cJSON *json = cJSON_Parse(line);
                    if (json) {
                        cJSON *p = cJSON_GetObjectItem(json, "p");
                        if (cJSON_IsNumber(p)) {
                            // Enviar el dato extraido a traves de Bluetooth
                            send_ble_data(p->valuedouble);
                            
                            // Pausa critica para no saturar el buffer del movil Android/iOS
                            vTaskDelay(pdMS_TO_TICKS(30)); 
                        }
                        cJSON_Delete(json);
                    }
                }
                fclose(f);
            } else {
                ESP_LOGE(TAG, "⚠️ No hay archivo data.json (Memoria vacía)");
            }
            
            // Indicar a la aplicacion movil que la descarga ha terminado
            send_ble_end_marker();
            is_spiffs_dump_requested = false;
        }

        // --- SECCION 3: CICLO DE LECTURA DE PRESION ---
        float current_pressure = read_pressure();

        if (is_sensor_active) {
            ESP_LOGI(TAG, "📡 Lectura en vivo: %.2f mB", current_pressure);
            
            // Transmitir inmediatamente la presion instantanea
            send_ble_data(current_pressure); 
            
            // Acumular el valor para sacar una media estable
            pressure_sum += current_pressure;
            reading_count++;

            // Cuando llegamos a 20 muestras (2 segundos a 100ms)
            if (reading_count >= 20) { 
                float average_pressure = pressure_sum / 20.0;
                
                // Guardamos el promedio estable en la Flash (Caja negra)
                save_reading_json(average_pressure);
                
                // Reiniciamos los contadores para el proximo bloque
                pressure_sum = 0.0; 
                reading_count = 0;
            }
        } else {
            // Si el sensor fue apagado por Bluetooth, purgar los contadores 
            // para evitar medias erroneas al volver a encenderlo.
            pressure_sum = 0.0; 
            reading_count = 0;
        }

        // Repetir el bucle cada 100 milisegundos (10 Hercios)
        vTaskDelay(pdMS_TO_TICKS(100)); 
    }
}