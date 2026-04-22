#include "bluetooth_manager.h"
#include "esp_log.h"
#include <string.h>

// Librerias nativas de NimBLE
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

// Instanciacion real de las variables globales
bool is_sensor_active = false;
bool is_spiffs_dump_requested = false;
bool is_spiffs_clear_requested = false;

// Variables de estado OTA
static esp_ota_handle_t ota_handle = 0;
static const esp_partition_t *update_partition = NULL;

// Variables internas de conexion BLE
static uint16_t gatt_pressure_handle;
static uint16_t ble_connection_handle = 0xFFFF;

static uint8_t ota_buffer[8192]; 
static int ota_buffer_idx = 0;

// ---------------------------------------------------------
// OPTIMIZACIÓN DE VELOCIDAD
// ---------------------------------------------------------
void request_fast_connection(uint16_t conn_handle)
{
    struct ble_gap_upd_params params;
    params.itvl_min = 6; // 7.5ms (El mínimo absoluto)
    params.itvl_max = 8; // 10ms
    params.latency = 0;
    params.supervision_timeout = 500;
    ble_gap_update_params(conn_handle, &params);
}

// ---------------------------------------------------------
// CALLBACKS DE LECTURA/ESCRITURA (GATT)
// ---------------------------------------------------------

static int gatt_ota_data_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR)
    {
        if (ota_handle != 0)
        {
            uint16_t len = ctxt->om->om_len;
            uint8_t *data = ctxt->om->om_data;

            // Copiamos los datos recibidos al final del buffer actual
            memcpy(&ota_buffer[ota_buffer_idx], data, len);
            ota_buffer_idx += len;

            // Si tenemos un sector completo de 4KB (tamaño estándar de Flash)
            if (ota_buffer_idx >= 4096)
            {
                // Escribimos los 4KB en Flash
                esp_ota_write(ota_handle, ota_buffer, 4096);
                
                // Calculamos cuánto sobró
                int remaining = ota_buffer_idx - 4096;
                
                // Si quedaron datos, los movemos al inicio del buffer
                if (remaining > 0) {
                    // Usamos punteros para mover los datos restantes al inicio (índice 0)
                    memmove(ota_buffer, &ota_buffer[4096], remaining);
                }
                
                // Actualizamos el índice
                ota_buffer_idx = remaining;
            }
        }
    }
    return 0;
}

static int gatt_svr_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR)
    {
        uint8_t cmd = ctxt->om->om_data[0];

        if (cmd == 0)
        {
            is_sensor_active = false;
            ESP_LOGI(TAG, "Comando: APAGAR EN VIVO");
        }
        else if (cmd == 1)
        {
            is_sensor_active = true;
            ESP_LOGI(TAG, "Comando: ACTIVAR EN VIVO");
        }
        else if (cmd == 2)
        {
            is_spiffs_dump_requested = true;
            ESP_LOGI(TAG, "Comando: DESCARGAR SPIFFS");
        }
        else if (cmd == 3)
        {
            is_spiffs_clear_requested = true;
            ESP_LOGI(TAG, "Comando: BORRAR SPIFFS");
        }
        else if (cmd == 4)
        {
            ESP_LOGI(TAG, "Iniciando proceso OTA...");
            is_sensor_active = false;
            update_partition = esp_ota_get_next_update_partition(NULL);
            if (update_partition != NULL)
            {
                esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
            }
        }
        else if (cmd == 5)
        {
            // Escribir lo que quede en el buffer antes de cerrar
            if (ota_buffer_idx > 0)
            {
                esp_ota_write(ota_handle, ota_buffer, ota_buffer_idx);
            }
            ESP_LOGI(TAG, "Finalizando OTA...");
            if (esp_ota_end(ota_handle) == ESP_OK)
            {
                esp_ota_set_boot_partition(update_partition);
                esp_restart();
            }
            ota_handle = 0;
            ota_buffer_idx = 0; // Reset
        }
    }
    return 0;
}

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY,
     .uuid = BLE_UUID128_DECLARE(0x78, 0x56, 0x34, 0x12, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12, 0x78, 0x56),
     .characteristics = (struct ble_gatt_chr_def[]){
         {
             .uuid = BLE_UUID128_DECLARE(0x87, 0x65, 0x43, 0x21, 0x87, 0x65, 0x43, 0x21, 0x87, 0x65, 0x43, 0x21, 0x87, 0x65, 0x43, 0x21),
             .access_cb = gatt_svr_access,
             .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
             .val_handle = &gatt_pressure_handle,
         },
         {
             .uuid = BLE_UUID128_DECLARE(0x87, 0x65, 0x43, 0x21, 0x87, 0x65, 0x43, 0x21, 0x87, 0x65, 0x43, 0x21, 0x87, 0x65, 0x43, 0x31),
             .access_cb = gatt_ota_data_access,
             .flags = BLE_GATT_CHR_F_WRITE_NO_RSP,
         },
         {0}}},
    {0}};

// ---------------------------------------------------------
// CONTROLADORES DE CONEXIÓN
// ---------------------------------------------------------

void ble_app_on_sync(void);

static int ble_gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type)
    {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0)
        {
            ble_connection_handle = event->connect.conn_handle;
            // Solicitamos prioridad alta apenas se conecta
            request_fast_connection(ble_connection_handle);
        }
        else
        {
            ble_app_on_sync();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ble_connection_handle = 0xFFFF;
        ble_app_on_sync();
        break;
    }
    return 0;
}

void ble_app_on_sync(void)
{
    uint8_t addr_type;
    ble_hs_id_infer_auto(0, &addr_type);
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof fields);
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)ble_svc_gap_device_name();
    fields.name_len = strlen((char *)fields.name);
    fields.name_is_complete = 1;
    ble_gap_adv_set_fields(&fields);
    struct ble_gap_adv_params adv_params = {.conn_mode = BLE_GAP_CONN_MODE_UND, .disc_mode = BLE_GAP_DISC_MODE_GEN};
    ble_gap_adv_start(addr_type, NULL, BLE_HS_FOREVER, &adv_params, ble_gap_event, NULL);
}

void host_task(void *param)
{
    nimble_port_run();
}

void initialize_bluetooth(void)
{
    nimble_port_init();
    ble_svc_gap_device_name_set("ESP32S3_PRESION");
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_gatts_count_cfg(gatt_svcs);
    ble_gatts_add_svcs(gatt_svcs);
    ble_hs_cfg.sync_cb = ble_app_on_sync;
    nimble_port_freertos_init(host_task);
}

void send_ble_data(float value)
{
    if (ble_connection_handle != 0xFFFF)
    {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(&value, sizeof(value));
        ble_gatts_notify_custom(ble_connection_handle, gatt_pressure_handle, om);
    }
}

void send_ble_end_marker(void)
{
    float end_marker = -9999.0;
    send_ble_data(end_marker);
}