#include "bluetooth_manager.h"
#include "esp_log.h"
#include <string.h>

// Librerias nativas de NimBLE (Bluetooth Low Energy)
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

// Librerias nativas para OTA
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"

static const char *TAG = "BLE_MGR";

// Instanciacion real de las variables globales que controlan la aplicacion
bool is_sensor_active = false;
bool is_spiffs_dump_requested = false;
bool is_spiffs_clear_requested = false;

// Variables de estado OTA
static esp_ota_handle_t ota_handle = 0;
static const esp_partition_t *update_partition = NULL;

// Variables internas de conexion BLE
static uint16_t gatt_pressure_handle;           
static uint16_t ble_connection_handle = 0xFFFF; // 0xFFFF significa "Desconectado"     


// ---------------------------------------------------------
// CALLBACKS DE LECTURA/ESCRITURA (GATT)
// ---------------------------------------------------------

// Callback exclusivo para recibir fragmentos del archivo .bin a maxima velocidad
static int gatt_ota_data_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        if (ota_handle != 0) {
            // Escribe directamente a la memoria Flash sin pasar por SPIFFS
            esp_err_t err = esp_ota_write(ota_handle, ctxt->om->om_data, ctxt->om->om_len);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Error escribiendo OTA: %s", esp_err_to_name(err));
            }
        }
    }
    return 0;
}

// Funcion de devolucion de llamada (Callback) cuando Angular escribe un dato (Comandos)
static int gatt_svr_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        // Leemos el primer byte enviado por la App Angular
        uint8_t cmd = ctxt->om->om_data[0];
        
        // Decodificamos el comando y cambiamos el estado global
        if (cmd == 0) {
            is_sensor_active = false;
            ESP_LOGI(TAG, "Comando recibido desde Angular: APAGAR EN VIVO");
        } else if (cmd == 1) {
            is_sensor_active = true;
            ESP_LOGI(TAG, "Comando recibido desde Angular: ACTIVAR EN VIVO");
        } else if (cmd == 2) {
            is_spiffs_dump_requested = true;
            ESP_LOGI(TAG, "Comando recibido desde Angular: DESCARGAR SPIFFS");
        } else if (cmd == 3) {
            is_spiffs_clear_requested = true;
            ESP_LOGI(TAG, "Comando recibido desde Angular: BORRAR SPIFFS");
        } else if (cmd == 4) {
            ESP_LOGI(TAG, "Iniciando proceso OTA...");
            is_sensor_active = false; // Pausamos lecturas por seguridad
            update_partition = esp_ota_get_next_update_partition(NULL);
            if (update_partition != NULL) {
                esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
                ESP_LOGI(TAG, "Particion OTA lista.");
            } else {
                ESP_LOGE(TAG, "No se encontro particion OTA valida.");
            }
        } else if (cmd == 5) {
            ESP_LOGI(TAG, "Finalizando OTA y reiniciando...");
            if (esp_ota_end(ota_handle) == ESP_OK) {
                esp_err_t err = esp_ota_set_boot_partition(update_partition);
                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "OTA Exitoso! Reiniciando en 1 seg...");
                    vTaskDelay(1000 / portTICK_PERIOD_MS);
                    esp_restart();
                } else {
                    ESP_LOGE(TAG, "Fallo al asignar particion de booteo");
                }
            } else {
                ESP_LOGE(TAG, "Fallo en verificación de imagen OTA. (Corrupto)");
            }
            ota_handle = 0; // Reseteamos handle por seguridad
        }
    }
    return 0;
}


// ---------------------------------------------------------
// DEFINICIÓN DE SERVICIOS Y CARACTERÍSTICAS
// ---------------------------------------------------------

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY,
     // UUID del Servicio Principal
     .uuid = BLE_UUID128_DECLARE(0x78,0x56,0x34,0x12,0x34,0x12,0x78,0x56,0x34,0x12,0x78,0x56,0x34,0x12,0x78,0x56),
     .characteristics = (struct ble_gatt_chr_def[]) {
         {
          // CARACTERÍSTICA 1 (Comandos generales y Notificaciones del Sensor)
          .uuid = BLE_UUID128_DECLARE(0x87,0x65,0x43,0x21,0x87,0x65,0x43,0x21,0x87,0x65,0x43,0x21,0x87,0x65,0x43,0x21),
          .access_cb = gatt_svr_access, 
          .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
          .val_handle = &gatt_pressure_handle, 
         },
         {
          // CARACTERÍSTICA 2 (Exclusiva para recibir fragmentos OTA rápidamente sin respuesta)
          .uuid = BLE_UUID128_DECLARE(0x87,0x65,0x43,0x21,0x87,0x65,0x43,0x21,0x87,0x65,0x43,0x21,0x87,0x65,0x43,0x31),
          .access_cb = gatt_ota_data_access,
          .flags = BLE_GATT_CHR_F_WRITE_NO_RSP, 
         }, 
         {0}}}, {0}};


// ---------------------------------------------------------
// CONTROLADORES DE CONEXIÓN Y ANUNCIAMIENTO
// ---------------------------------------------------------

// Pre-declaracion necesaria para NimBLE
void ble_app_on_sync(void); 

// Callback que maneja cuando el movil se conecta o se desconecta
static int ble_gap_event(struct ble_gap_event *event, void *arg) {
    if (event->type == BLE_GAP_EVENT_CONNECT) {
        if (event->connect.status == 0) {
            ESP_LOGI(TAG, "✅ ¡Móvil conectado!");
            ble_connection_handle = event->connect.conn_handle; // Guardamos quien se ha conectado
        } else {
            ble_app_on_sync();
        }
    } else if (event->type == BLE_GAP_EVENT_DISCONNECT) {
        ESP_LOGI(TAG, "❌ ¡Móvil desconectado! Reactivando la antena...");
        ble_connection_handle = 0xFFFF; // Marcamos como desconectado
        ble_app_on_sync(); // Volvemos a anunciar
        is_sensor_active = false; // Pausamos por seguridad
    }
    return 0;
}

// Inicia el proceso de anunciamiento (Advertising) para aparecer en el escaner del movil
void ble_app_on_sync(void) {
    uint8_t addr_type; 
    ble_hs_id_infer_auto(0, &addr_type);
    
    struct ble_hs_adv_fields fields; 
    memset(&fields, 0, sizeof fields);
    
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)ble_svc_gap_device_name(); // "ESP32S3_PRESION"
    fields.name_len = strlen((char*)fields.name); 
    fields.name_is_complete = 1;
    
    ble_gap_adv_set_fields(&fields);
    
    struct ble_gap_adv_params adv_params = {.conn_mode = BLE_GAP_CONN_MODE_UND, .disc_mode = BLE_GAP_DISC_MODE_GEN};
    ble_gap_adv_start(addr_type, NULL, BLE_HS_FOREVER, &adv_params, ble_gap_event, NULL);
}

// Tarea principal de FreeRTOS para el host Bluetooth
void host_task(void *param) { 
    nimble_port_run(); 
}

void initialize_bluetooth(void) {
    nimble_port_init();
    ble_svc_gap_device_name_set("ESP32S3_PRESION");
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_gatts_count_cfg(gatt_svcs);
    ble_gatts_add_svcs(gatt_svcs);
    ble_hs_cfg.sync_cb = ble_app_on_sync;
    nimble_port_freertos_init(host_task);
}


// ---------------------------------------------------------
// FUNCIONES DE ENVÍO DE DATOS
// ---------------------------------------------------------

void send_ble_data(float value) {
    // Si hay un movil conectado, le enviamos el flotante por notificacion
    if (ble_connection_handle != 0xFFFF) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(&value, sizeof(value));
        ble_gatts_notify_custom(ble_connection_handle, gatt_pressure_handle, om);
    }
}

void send_ble_end_marker(void) {
    float end_marker = -9999.0;
    send_ble_data(end_marker);
}