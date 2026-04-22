#include "bluetooth_manager.h"
#include "esp_log.h"
#include <string.h>

// Librerias nativas de FreeRTOS para procesamiento asíncrono
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"

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

// ---------------------------------------------------------
// VARIABLES OTA ASÍNCRONO (FreeRTOS)
// ---------------------------------------------------------
static RingbufHandle_t ota_ringbuf = NULL;
static TaskHandle_t ota_task_handle = NULL;
volatile bool ota_is_finished = false;
volatile bool ota_task_exited = true; 

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
// TAREA DE ESCRITURA EN FLASH EN SEGUNDO PLANO
// ---------------------------------------------------------
void ota_writer_task(void *pvParameter)
{
    uint8_t write_buf[4096];
    int write_idx = 0;

    ESP_LOGI(TAG, "Tarea OTA: Iniciada y esperando datos...");

    while (1)
    {
        size_t item_size = 0;
        // Esperamos datos del Ringbuffer. Si no hay, despierta cada 50ms para comprobar
        uint8_t *item = (uint8_t *)xRingbufferReceiveUpTo(ota_ringbuf, &item_size, pdMS_TO_TICKS(50), 4096 - write_idx);

        if (item != NULL)
        {
            // Copiamos los datos extraídos del ringbuffer al buffer de página local (write_buf)
            memcpy(&write_buf[write_idx], item, item_size);
            write_idx += item_size;
            
            // Liberamos la memoria del item en el Ringbuffer para que Bluetooth pueda seguir llenando
            vRingbufferReturnItem(ota_ringbuf, (void *)item);

            // Si juntamos 4KB exactos, escribimos en la Flash
            if (write_idx == 4096)
            {
                esp_ota_write(ota_handle, write_buf, 4096);
                write_idx = 0;
            }
        }
        else
        {
            // Si item es NULL (no llegaron datos en los ultimos 50ms) y la app pidió terminar
            if (ota_is_finished)
            {
                break; // Rompemos el bucle infinito para finalizar
            }
        }
    }

    // Escribimos cualquier dato residual que haya quedado en el buffer (menor a 4KB)
    if (write_idx > 0)
    {
        esp_ota_write(ota_handle, write_buf, write_idx);
    }

    ESP_LOGI(TAG, "Tarea OTA: Escritura en Flash finalizada con éxito.");
    
    // Indicamos a gatt_svr_access que ya terminamos de guardar todo
    ota_task_exited = true; 
    vTaskDelete(NULL); // Nos autodestruimos
}


// ---------------------------------------------------------
// CALLBACKS DE LECTURA/ESCRITURA (GATT)
// ---------------------------------------------------------

static int gatt_ota_data_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR)
    {
        if (ota_handle != 0 && ota_ringbuf != NULL)
        {
            // ---> EL SECRETO: Obtener la longitud TOTAL de todos los "vagones" encadenados
            uint16_t total_len = OS_MBUF_PKTLEN(ctxt->om);
            
            if (total_len > 0) 
            {
                // Creamos un buffer temporal para unir todos los datos
                uint8_t temp_buf[512]; 
                
                // Copiamos TODOS los fragmentos del paquete en nuestro buffer
                os_mbuf_copydata(ctxt->om, 0, total_len, temp_buf);

                // Enviamos el paquete gigante completo al Ringbuffer
                BaseType_t res = xRingbufferSend(ota_ringbuf, temp_buf, total_len, pdMS_TO_TICKS(50));
                
                if (res != pdTRUE)
                {
                    ESP_LOGE(TAG, "⚠️ Ringbuffer OTA lleno! Perdiendo %d bytes.", total_len);
                }
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
            ESP_LOGI(TAG, "Preparando OTA de alta velocidad...");
            is_sensor_active = false;
            update_partition = esp_ota_get_next_update_partition(NULL);
            
            if (update_partition != NULL)
            {
                // 1. Iniciamos partición OTA (Esto puede tomar algo de tiempo)
                esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);

                // 2. Creamos Ringbuffer en RAM de 16 KB (TIPO BYTEBUF)
                if (ota_ringbuf != NULL) { vRingbufferDelete(ota_ringbuf); }
                ota_ringbuf = xRingbufferCreate(16384, RINGBUF_TYPE_BYTEBUF);

                // 3. Lanzamos la tarea de escritura en segundo plano
                ota_is_finished = false;
                ota_task_exited = false;
                
                // Creamos la tarea con prioridad 5 (ligeramente inferior a la de NimBLE)
                xTaskCreate(ota_writer_task, "ota_task", 8192, NULL, 5, &ota_task_handle);
                
                ESP_LOGI(TAG, "¡Listo para recibir paquetes!");
            }
        }
        else if (cmd == 5)
        {
            ESP_LOGI(TAG, "Recibida señal de fin de OTA. Esperando a vaciar RAM a Flash...");
            if (ota_handle != 0)
            {
                // 1. Avisamos a la tarea secundaria que no llegarán más paquetes
                ota_is_finished = true;
                
                // 2. Esperamos hasta que la tarea confirme que terminó de escribir el último byte
                while (!ota_task_exited) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                }

                // 3. Cerramos el proceso OTA de forma segura
                if (esp_ota_end(ota_handle) == ESP_OK)
                {
                    esp_ota_set_boot_partition(update_partition);
                    ESP_LOGI(TAG, "OTA Completo. Reiniciando en 1 segundo...");
                    
                    // Limpiamos la RAM
                    if (ota_ringbuf != NULL) {
                        vRingbufferDelete(ota_ringbuf);
                        ota_ringbuf = NULL;
                    }
                    
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    esp_restart();
                }
                else 
                {
                    ESP_LOGE(TAG, "Error finalizando OTA. Firma o tamaño incorrecto.");
                }
                
                ota_handle = 0;
            }
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
            request_fast_connection(ble_connection_handle);
            ble_gap_set_data_len(ble_connection_handle, 251, 2120);
            
            // ---> ¡NUEVO: EL ESP32 FUERZA AL MÓVIL A AMPLIAR EL MTU! <---
            // Esto obliga a Android/iOS a aceptar nuestros paquetes gigantes de 490 bytes
            ble_gattc_exchange_mtu(ble_connection_handle, NULL, NULL);
        }
        else
        {
            ble_app_on_sync();
        }
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
    ble_att_set_preferred_mtu(512);
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