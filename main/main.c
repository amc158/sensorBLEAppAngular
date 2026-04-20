#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_spiffs.h"
#include "nvs_flash.h"
#include "cJSON.h"

// NimBLE (Bluetooth LE)
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "CEREBRO_SENSOR_BLE";

#define SENSOR_ADC_CHANNEL ADC_CHANNEL_3 // GPIO 4
float voltaje_zero_dinamico = 0.502;    
float presion_filtrada = 0;
bool sensor_active = false;

uint16_t gatt_presion_handle;           
uint16_t ble_conn_handle = 0xFFFF;      
bool dump_spiffs_requested = false;
bool clear_spiffs_requested = false;

void guardar_lectura_json(float valor) {
    FILE* f = fopen("/spiffs/data.json", "a");
    if (f == NULL) return;
    
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "p", valor);
    char *json_str = cJSON_PrintUnformatted(root);
    fprintf(f, "%s\n", json_str);
    
    ESP_LOGI(TAG, "💾 Media guardada en SPIFFS: %.2f mB", valor);

    free(json_str);
    cJSON_Delete(root);
    fclose(f);
}

static int gatt_svr_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint8_t cmd = ctxt->om->om_data[0];
        if (cmd == 0) {
            sensor_active = false;
            ESP_LOGI(TAG, "Comando recibido: APAGAR EN VIVO");
        } else if (cmd == 1) {
            sensor_active = true;
            ESP_LOGI(TAG, "Comando recibido: ACTIVAR EN VIVO");
        } else if (cmd == 2) {
            dump_spiffs_requested = true;
            ESP_LOGI(TAG, "Comando recibido: DESCARGAR SPIFFS");
        } else if (cmd == 3) {
            clear_spiffs_requested = true;
            ESP_LOGI(TAG, "Comando recibido: BORRAR SPIFFS");
        }
    }
    return 0;
}

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY,
     .uuid = BLE_UUID128_DECLARE(0x78,0x56,0x34,0x12,0x34,0x12,0x78,0x56,0x34,0x12,0x78,0x56,0x34,0x12,0x78,0x56),
     .characteristics = (struct ble_gatt_chr_def[]) {
         {.uuid = BLE_UUID128_DECLARE(0x87,0x65,0x43,0x21,0x87,0x65,0x43,0x21,0x87,0x65,0x43,0x21,0x87,0x65,0x43,0x21),
          .access_cb = gatt_svr_access,
          .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
          .val_handle = &gatt_presion_handle,
         }, {0}}},
    {0}};

void ble_app_on_sync(void); 

static int ble_gap_event(struct ble_gap_event *event, void *arg) {
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                ESP_LOGI(TAG, "✅ ¡Móvil conectado!");
                ble_conn_handle = event->connect.conn_handle; 
            } else {
                ble_app_on_sync();
            }
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "❌ ¡Móvil desconectado! Reactivando antena...");
            ble_conn_handle = 0xFFFF; 
            ble_app_on_sync(); 
            sensor_active = false; 
            break;
    }
    return 0;
}

void ble_app_on_sync(void) {
    uint8_t addr_type;
    ble_hs_id_infer_auto(0, &addr_type);
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof fields);
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    const char *name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;
    ble_gap_adv_set_fields(&fields);
    struct ble_gap_adv_params adv_params = {.conn_mode = BLE_GAP_CONN_MODE_UND, .disc_mode = BLE_GAP_DISC_MODE_GEN};
    ble_gap_adv_start(addr_type, NULL, BLE_HS_FOREVER, &adv_params, ble_gap_event, NULL);
}

void host_task(void *param) { nimble_port_run(); }

void app_main(void) {
    nvs_flash_init();
    esp_vfs_spiffs_conf_t spiffs_cfg = {.base_path = "/spiffs", .partition_label = "storage", .max_files = 5, .format_if_mount_failed = true};
    esp_vfs_spiffs_register(&spiffs_cfg);

    adc_oneshot_unit_handle_t adc1_handle;
    adc_oneshot_unit_init_cfg_t init_cfg = { .unit_id = ADC_UNIT_1 };
    adc_oneshot_new_unit(&init_cfg, &adc1_handle);
    adc_oneshot_chan_cfg_t adc_config = { .bitwidth = ADC_BITWIDTH_DEFAULT, .atten = ADC_ATTEN_DB_12 };
    adc_oneshot_config_channel(adc1_handle, SENSOR_ADC_CHANNEL, &adc_config);

    float suma = 0;
    for(int i=0; i<40; i++) {
        int r; adc_oneshot_read(adc1_handle, SENSOR_ADC_CHANNEL, &r);
        suma += (r / 4095.0) * 3.3 * 1.5;
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    voltaje_zero_dinamico = suma / 40.0;

    nimble_port_init();
    ble_svc_gap_device_name_set("ESP32S3_PRESION");
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_gatts_count_cfg(gatt_svcs);
    ble_gatts_add_svcs(gatt_svcs);
    ble_hs_cfg.sync_cb = ble_app_on_sync;
    nimble_port_freertos_init(host_task);

    float alfa = 0.15;
    
    // ¡NUEVAS VARIABLES PARA CALCULAR LA MEDIA!
    int contador_lecturas = 0;
    float suma_presion = 0;

    while (1) {
        if (clear_spiffs_requested) {
            remove("/spiffs/data.json");
            clear_spiffs_requested = false;
            ESP_LOGW(TAG, "🗑️ ¡Memoria SPIFFS borrada por completo!");
        }

        if (dump_spiffs_requested) {
            sensor_active = false; 
            // Vaciamos el acumulador por si se quedó a medias
            contador_lecturas = 0; 
            suma_presion = 0;

            FILE* f = fopen("/spiffs/data.json", "r");
            if (f != NULL) {
                char line[64];
                ESP_LOGI(TAG, "🚀 Iniciando descarga a la App...");
                while (fgets(line, sizeof(line), f)) {
                    cJSON *json = cJSON_Parse(line);
                    if (json) {
                        cJSON *p = cJSON_GetObjectItem(json, "p");
                        if (cJSON_IsNumber(p)) {
                            float val = p->valuedouble;
                            ESP_LOGI(TAG, "📤 Enviando historial: %.2f", val);

                            if (ble_conn_handle != 0xFFFF) {
                                struct os_mbuf *om = ble_hs_mbuf_from_flat(&val, sizeof(val));
                                ble_gatts_notify_custom(ble_conn_handle, gatt_presion_handle, om);
                                vTaskDelay(pdMS_TO_TICKS(30)); 
                            }
                        }
                        cJSON_Delete(json);
                    }
                }
                fclose(f);
            } else {
                ESP_LOGE(TAG, "⚠️ No hay archivo data.json (Memoria vacía)");
            }
            
            float end_marker = -9999.0;
            if (ble_conn_handle != 0xFFFF) {
                struct os_mbuf *om = ble_hs_mbuf_from_flat(&end_marker, sizeof(end_marker));
                ble_gatts_notify_custom(ble_conn_handle, gatt_presion_handle, om);
            }
            ESP_LOGI(TAG, "🏁 Descarga finalizada.");
            dump_spiffs_requested = false;
        }

        int raw;
        adc_oneshot_read(adc1_handle, SENSOR_ADC_CHANNEL, &raw);
        float v = ((raw / 4095.0) * 3.3) * 1.5;
        float p_inst = (v - voltaje_zero_dinamico) * -250.0;
        
        presion_filtrada = (alfa * p_inst) + ((1.0 - alfa) * presion_filtrada);

        if (sensor_active) {
            ESP_LOGI(TAG, "📡 Lectura en vivo: %.2f mB", presion_filtrada);

            // 1. Enviar el dato en tiempo real a la app
            if (ble_conn_handle != 0xFFFF) {
                struct os_mbuf *om = ble_hs_mbuf_from_flat(&presion_filtrada, sizeof(presion_filtrada));
                ble_gatts_notify_custom(ble_conn_handle, gatt_presion_handle, om);
            }
            
            // 2. Acumular para calcular la media
            suma_presion += presion_filtrada;
            contador_lecturas++;

            // 3. Cuando llegamos a 20 lecturas (2 segundos)
            if (contador_lecturas >= 20) { 
                float media_calculada = suma_presion / 20.0;
                guardar_lectura_json(media_calculada);
                
                // Resetear los contadores para los próximos 2 segundos
                suma_presion = 0;
                contador_lecturas = 0;
            }
        } else {
            // Si el sensor se apaga, reseteamos el cálculo para no arrastrar basura
            suma_presion = 0;
            contador_lecturas = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(100)); // Bucle de 10Hz
    }
}