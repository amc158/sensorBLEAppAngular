#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "cJSON.h"

// #include "esp_random.h" // Libreria para engañar al compilador

// Archivos de Cabecera (Librerías Modulares)
#include "spiffs_manager.h"
#include "pressure_sensor.h"
#include "bluetooth_manager.h"

static const char *TAG = "MAIN_APP";

// --- INICIO DEL RELLENO MASIVO PARA OTA ---
// Creamos un array de 600,000 bytes (600 KB) llenos del valor hexadecimal 0x02.
// const uint8_t dummy_firmware_payload[600000] __attribute__((used)) = { 
//     [0 ... 599999] = 0x02 
// };
// --- FIN DEL RELLENO ---

void app_main(void) {
    // // 1. Generamos un número aleatorio entre 0 y 599999
    // uint32_t indice_aleatorio = esp_random() % 600000;
    
    // // 2. Leemos el array usando ese índice. Al ser 'volatile', el compilador no puede borrarlo.
    // volatile uint8_t byte_leido = dummy_firmware_payload[indice_aleatorio];

    // // Mensaje destacado de la V2 en la terminal
    // ESP_LOGI(TAG, "=============================================");
    // ESP_LOGI(TAG, " 🚀 INICIANDO FIRMWARE: VERSION 2 ");
    // ESP_LOGI(TAG, " 📦 Tamaño de relleno OTA: %zu bytes", sizeof(dummy_firmware_payload));
    // ESP_LOGI(TAG, " 🔒 Test anti-borrado (Byte %lu): %02X", indice_aleatorio, byte_leido);
    // ESP_LOGI(TAG, "=============================================");

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