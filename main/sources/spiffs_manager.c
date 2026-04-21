#include "spiffs_manager.h"
#include "esp_spiffs.h"
#include "esp_log.h"
#include "cJSON.h"
#include <stdio.h>

static const char *TAG = "SPIFFS_MGR";

void initialize_spiffs(void) {
    // Configuracion de la particion SPIFFS
    esp_vfs_spiffs_conf_t spiffs_cfg = {
        .base_path = "/spiffs",             // Ruta donde se montara en el codigo
        .partition_label = "spiffs",        // <--- ¡CORREGIDO! Ahora coincide con partitions.csv
        .max_files = 5,                     // Maximo de archivos abiertos a la vez
        .format_if_mount_failed = true      // Formatear si es la primera vez que se usa
    };
    
    // Registramos e inicializamos la particion
    esp_err_t ret = esp_vfs_spiffs_register(&spiffs_cfg);
    
    // Validamos que se haya montado correctamente
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Fallo al montar o formatear SPIFFS");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Fallo al encontrar la particion SPIFFS");
        } else {
            ESP_LOGE(TAG, "Error al inicializar SPIFFS (%s)", esp_err_to_name(ret));
        }
        return;
    }
    
    ESP_LOGI(TAG, "✅ Memoria SPIFFS inicializada correctamente.");
}

void save_reading_json(float value) {
    // Abrimos el archivo en modo "a" (append) para añadir datos al final
    FILE* f = fopen("/spiffs/data.json", "a");
    if (f == NULL) {
        ESP_LOGE(TAG, "Error al abrir data.json para escribir.");
        return; 
    }
    
    // Creamos el objeto JSON y le añadimos la clave "p" (presion)
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "p", value);
    
    // Convertimos el objeto JSON a texto (string) sin formateo para ahorrar espacio
    char *json_str = cJSON_PrintUnformatted(root);
    
    // Escribimos la linea en el archivo fisico seguida de un salto de linea
    fprintf(f, "%s\n", json_str);
    
    ESP_LOGI(TAG, "💾 Media guardada en SPIFFS: %.2f mB", value);

    // Liberamos la memoria RAM para evitar fugas de memoria (Memory Leaks)
    free(json_str);
    cJSON_Delete(root);
    fclose(f);
}

void clear_spiffs_memory(void) {
    // Borramos fisicamente el archivo del chip
    if (remove("/spiffs/data.json") == 0) {
        ESP_LOGW(TAG, "🗑️ Archivo data.json eliminado de la memoria flash.");
    } else {
        ESP_LOGE(TAG, "No se pudo borrar data.json (quizás ya estaba vacío).");
    }
}