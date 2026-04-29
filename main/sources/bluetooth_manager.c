#include "bluetooth_manager.h"
#include "esp_log.h"
#include <string.h>

// Librerias nativas de FreeRTOS para procesamiento asíncrono
// Son esenciales para desacoplar la recepción de datos Bluetooth (rápida) 
// de la escritura en memoria Flash (lenta).
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"

// Librerias nativas de NimBLE (Pila Bluetooth de Apache adoptada por Espressif)
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
// ota_handle: Actúa como el "puntero de archivo" donde se está guardando el firmware.
// update_partition: Apunta al espacio físico de la Flash asignado para el nuevo OTA.
static esp_ota_handle_t ota_handle = 0;
static const esp_partition_t *update_partition = NULL;

// Variables internas de conexion BLE
static uint16_t gatt_pressure_handle;
static uint16_t ble_connection_handle = 0xFFFF; // 0xFFFF significa "Desconectado"

// ---------------------------------------------------------
// VARIABLES OTA ASÍNCRONO (FreeRTOS)
// ---------------------------------------------------------
// El Ringbuffer es una estructura de memoria circular (First-In-First-Out).
// Actúa como "amortiguador" entre NimBLE y la Flash.
static RingbufHandle_t ota_ringbuf = NULL;
static TaskHandle_t ota_task_handle = NULL;

// Flags volátiles porque se leen/escriben desde diferentes hilos (tareas) concurrentes.
volatile bool ota_is_finished = false;
volatile bool ota_task_exited = true; 

// ---------------------------------------------------------
// OPTIMIZACIÓN DE VELOCIDAD FÍSICA
// ---------------------------------------------------------
void request_fast_connection(uint16_t conn_handle)
{
    // Pedimos al móvil o PC que acorte el tiempo entre ventanas de comunicación de radio.
    // Bajar el intervalo aumenta el consumo de batería pero maximiza el ancho de banda.
    struct ble_gap_upd_params params;
    params.itvl_min = 6; // 6 * 1.25ms = 7.5ms (Es el mínimo absoluto permitido por Apple/Android)
    params.itvl_max = 8; // 8 * 1.25ms = 10ms
    params.latency = 0;  // Latencia de esclavo a 0: el ESP32 debe responder siempre en cada ventana
    params.supervision_timeout = 500;
    ble_gap_update_params(conn_handle, &params);
}

// ---------------------------------------------------------
// TAREA DE ESCRITURA EN FLASH EN SEGUNDO PLANO
// ---------------------------------------------------------

// Tarea independiente para reiniciar el chip sin bloquear el Bluetooth
void delayed_reboot_task(void *pvParameter)
{
    vTaskDelay(pdMS_TO_TICKS(1000)); // Espera 1 segundo
    esp_restart();                   // Reinicia el chip
}

// Esta función corre en su propio núcleo/hilo, separada del hilo de Bluetooth.
void ota_writer_task(void *pvParameter)
{
    // Buffer local de 4KB (4096 bytes). 
    // La memoria Flash se borra y escribe físicamente en "páginas" de 4KB.
    // Escribir de a 4KB es la forma más rápida y eficiente para el hardware del ESP32.
    uint8_t write_buf[4096];
    int write_idx = 0;

    ESP_LOGI(TAG, "Tarea OTA: Iniciada y esperando datos...");

    while (1)
    {
        size_t item_size = 0;
        
        // Bloqueo eficiente: La tarea se duerme (consume 0 CPU) hasta que llegue un dato 
        // al Ringbuffer, o hasta un máximo de 50ms (pdMS_TO_TICKS(50)).
        // Extrae solo la cantidad que cabe en el espacio restante de write_buf (4096 - write_idx).
        uint8_t *item = (uint8_t *)xRingbufferReceiveUpTo(ota_ringbuf, &item_size, pdMS_TO_TICKS(50), 4096 - write_idx);

        if (item != NULL)
        {
            // Copiamos los datos extraídos del ringbuffer al buffer de página local (write_buf)
            memcpy(&write_buf[write_idx], item, item_size);
            write_idx += item_size;
            
            // ¡Crucial! Le decimos al Ringbuffer que ya leímos esos datos, liberando 
            // ese espacio en la RAM para que la interrupción Bluetooth pueda seguir escribiendo.
            vRingbufferReturnItem(ota_ringbuf, (void *)item);

            // Cuando llenamos exactamente los 4KB, volcamos a Flash
            if (write_idx == 4096)
            {
                // --- SOLUCIÓN: CONTROL DE ERRORES DE ESCRITURA ---
                esp_err_t err = esp_ota_write(ota_handle, write_buf, 4096);
                if (err != ESP_OK) 
                {
                    ESP_LOGE(TAG, "Error fatal de escritura en Flash (%s). Abortando tarea...", esp_err_to_name(err));
                    // Si el archivo está corrupto, paramos de escribir inmediatamente
                    // para no inundar el log ni destrozar la partición inútilmente.
                    ota_is_finished = true; 
                    break; 
                }
                write_idx = 0;
            }
        }
        else
        {
            // Si pasaron 50ms sin recibir datos (item == NULL) y se activó la bandera de fin,
            // rompemos el bucle infinito para iniciar el apagado seguro de la tarea.
            if (ota_is_finished)
            {
                break; 
            }
        }
    }

    // Al finalizar el bucle, volcamos cualquier dato que haya quedado "huérfano"
    // en el buffer y no haya llegado a los 4KB completos.
    if (write_idx > 0)
    {
        esp_ota_write(ota_handle, write_buf, write_idx);
    }

    ESP_LOGI(TAG, "Tarea OTA: Escritura en Flash finalizada con éxito.");
    
    // Levantamos bandera para avisar al hilo de Bluetooth (comando 5) que ya puede reiniciar.
    ota_task_exited = true; 
    vTaskDelete(NULL); // La tarea se autodestruye liberando su memoria.
}

// ---------------------------------------------------------
// CALLBACKS DE LECTURA/ESCRITURA (GATT)
// ---------------------------------------------------------

// Este callback es disparado por NimBLE cuando el móvil escribe en la característica OTA.
// ¡ATENCIÓN! Este código corre en el hilo del Bluetooth. DEBE ser extremadamente rápido
// y no tener pausas bloqueantes (ni vTaskDelay, ni esp_ota_write).
static int gatt_ota_data_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR)
    {
        if (ota_handle != 0 && ota_ringbuf != NULL)
        {
            // ---> ENSAMBLAJE DE MBUFS (EL SECRETO DEL MTU GRANDE) <---
            // NimBLE no guarda paquetes >292 bytes en memoria contigua, sino en una lista enlazada (mbufs).
            // OS_MBUF_PKTLEN extrae la suma total de todos los fragmentos encadenados.
            uint16_t total_len = OS_MBUF_PKTLEN(ctxt->om);
            
            if (total_len > 0) 
            {
                // Buffer en la pila para ensamblar el paquete (máx esperado 512)
                uint8_t temp_buf[512]; 
                
                // Extrae e inserta ordenadamente todos los fragmentos dispersos en temp_buf
                os_mbuf_copydata(ctxt->om, 0, total_len, temp_buf);

                // Insertamos el paquete completo en el Ringbuffer de FreeRTOS.
                // Si el Ringbuffer está lleno (Flash muy lenta), el hilo de BT esperará 
                // hasta 50ms a que la tarea de Flash libere espacio.
                BaseType_t res = xRingbufferSend(ota_ringbuf, temp_buf, total_len, pdMS_TO_TICKS(50));
                
                if (res != pdTRUE)
                {
                    // Si falla, los datos se pierden corrompiendo el binario. 
                    ESP_LOGE(TAG, "⚠️ Ringbuffer OTA lleno! Perdiendo %d bytes.", total_len);
                }
            }
        }
    }
    return 0;
}

// Callback para la característica estándar. Maneja comandos (1 byte)
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
            
            // Busca la partición inactiva (la que NO está corriendo el firmware actual)
            update_partition = esp_ota_get_next_update_partition(NULL);
            
            if (update_partition != NULL)
            {
                // 1. Iniciamos partición OTA. Esto formatea los primeros sectores de Flash. Toma tiempo.
                esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);

                // 2. Destruimos si existía uno previo, y creamos un Ringbuffer en RAM de 16 KB.
                if (ota_ringbuf != NULL) { vRingbufferDelete(ota_ringbuf); }
                ota_ringbuf = xRingbufferCreate(16384, RINGBUF_TYPE_BYTEBUF);

                // 3. Reiniciamos banderas para la tarea asíncrona.
                ota_is_finished = false;
                ota_task_exited = false;
                
                // 4. Lanzamos la tarea de escritura en el FreeRTOS scheduler.
                // Prioridad 5 (la del BT de NimBLE suele ser altísima, esto asegura que el BT 
                // reciba los datos antes, y esta tarea escriba cuando el BT esté libre).
                xTaskCreate(ota_writer_task, "ota_task", 8192, NULL, 5, &ota_task_handle);
                
                ESP_LOGI(TAG, "¡Listo para recibir paquetes!");
            }
        }
        else if (cmd == 5)
        {
            ESP_LOGI(TAG, "Recibida señal de fin de OTA. Esperando a vaciar RAM a Flash...");
            if (ota_handle != 0)
            {
                // 1. Mandamos señal de "no hay más datos" a la tarea asíncrona
                ota_is_finished = true;
                
                // 2. Bucle de espera bloqueante. Nos quedamos frenados aquí verificando cada 10ms 
                // hasta que la tarea ota_writer_task cambie ota_task_exited a true.
                while (!ota_task_exited) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                }

                // 3. Cerramos el proceso OTA.
                if (esp_ota_end(ota_handle) == ESP_OK)
                {
                    esp_ota_set_boot_partition(update_partition);
                    ESP_LOGI(TAG, "OTA Completo. Reiniciando en 1 segundo...");
                    
                    if (ota_ringbuf != NULL) {
                        vRingbufferDelete(ota_ringbuf);
                        ota_ringbuf = NULL;
                    }
                    
                    // --- LA MAGIA ---
                    // Lanzamos la tarea de reinicio en segundo plano.
                    // Esto permite que el hilo del Bluetooth no se bloquee, llegue
                    // al "return 0" de abajo y le mande el "OK" a la app de Angular.
                    xTaskCreate(delayed_reboot_task, "reboot_task", 2048, NULL, 5, NULL);
                }
                else 
                {
                    ESP_LOGE(TAG, "Error finalizando OTA. Firma o tamaño incorrecto.");
                    ota_handle = 0;
                    return 0x05; // Devolvemos error nativo al móvil
                }
                
                ota_handle = 0;
            }
        }
    }
    return 0;
}

// Estructura GATT: Define cómo se "ve" este dispositivo Bluetooth en el escáner.
static const struct ble_gatt_svc_def gatt_svcs[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY,
     .uuid = BLE_UUID128_DECLARE(0x78, 0x56, 0x34, 0x12, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12, 0x78, 0x56),
     .characteristics = (struct ble_gatt_chr_def[]){
         {
             // Característica base (Comandos y Sensor)
             .uuid = BLE_UUID128_DECLARE(0x87, 0x65, 0x43, 0x21, 0x87, 0x65, 0x43, 0x21, 0x87, 0x65, 0x43, 0x21, 0x87, 0x65, 0x43, 0x21),
             .access_cb = gatt_svr_access,
             // F_WRITE (Comandos con respuesta), F_READ (Sincronización Web), F_NOTIFY (Streaming del sensor)
             .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
             .val_handle = &gatt_pressure_handle,
         },
         {
             // Característica OTA
             .uuid = BLE_UUID128_DECLARE(0x87, 0x65, 0x43, 0x21, 0x87, 0x65, 0x43, 0x21, 0x87, 0x65, 0x43, 0x21, 0x87, 0x65, 0x43, 0x31),
             .access_cb = gatt_ota_data_access,
             // F_WRITE_NO_RSP: Clave para la velocidad. Permite lanzar paquetes sin requerir 
             // un "OK" (ACK) por cada bloque recibido por parte del esclavo.
             .flags = BLE_GATT_CHR_F_WRITE_NO_RSP,
         },
         {0}}},
    {0}};

// ---------------------------------------------------------
// CONTROLADORES DE CONEXIÓN
// ---------------------------------------------------------

void ble_app_on_sync(void);

// Gestor de eventos de estado de conexión GAP
static int ble_gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type)
    {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) // 0 significa conexión exitosa
        {
            ble_connection_handle = event->connect.conn_handle;
            request_fast_connection(ble_connection_handle);
            ble_gap_set_data_len(ble_connection_handle, 251, 2120);
            ble_gattc_exchange_mtu(ble_connection_handle, NULL, NULL);
        }
        else
        {
            ble_app_on_sync();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ble_connection_handle = 0xFFFF; // Marcamos como desconectado

        // --- SISTEMA DE LIMPIEZA ANTI-CORTES (NUEVO) ---
        // Si el ota_handle no es 0, significa que se cortó el Bluetooth en mitad de una descarga.
        if (ota_handle != 0) 
        {
            ESP_LOGW(TAG, "⚠️ Conexión perdida durante OTA. Abortando y limpiando sistema...");

            // 1. Mandamos la señal de apagado a la tarea que escribe en Flash
            ota_is_finished = true;

            // 2. Esperamos pacientemente a que la tarea termine lo que estaba haciendo y se suicide
            while (!ota_task_exited) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }

            // 3. Destruimos el proceso OTA a nivel de hardware.
            // Esto invalida la partición y borra los datos parciales para evitar corrupciones.
            esp_ota_abort(ota_handle);
            ota_handle = 0; // Reseteamos el puntero

            // 4. Limpiamos la memoria RAM (el Ringbuffer) para que no haya fugas
            if (ota_ringbuf != NULL) {
                vRingbufferDelete(ota_ringbuf);
                ota_ringbuf = NULL;
            }

            ESP_LOGI(TAG, "🧹 Limpieza OTA completada. Dispositivo listo de nuevo.");
        }

        // Volvemos a emitir publicidad para que el usuario pueda reconectarse
        ble_app_on_sync();
        break;
    }
    return 0;
}

// Inicia la publicación de la señal del ESP32 para ser descubierto.
void ble_app_on_sync(void)
{
    uint8_t addr_type;
    ble_hs_id_infer_auto(0, &addr_type);
    
    // Configuramos los campos del mensaje de "anuncio" (Publicidad)
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof fields);
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP; // Descubrible, sin soporte a BT Clásico
    fields.name = (uint8_t *)ble_svc_gap_device_name();
    fields.name_len = strlen((char *)fields.name);
    fields.name_is_complete = 1;
    ble_gap_adv_set_fields(&fields);
    
    // Comenzamos a anunciarnos continuamente
    struct ble_gap_adv_params adv_params = {.conn_mode = BLE_GAP_CONN_MODE_UND, .disc_mode = BLE_GAP_DISC_MODE_GEN};
    ble_gap_adv_start(addr_type, NULL, BLE_HS_FOREVER, &adv_params, ble_gap_event, NULL);
}

// El bucle principal del puerto NimBLE que corre en FreeRTOS
void host_task(void *param)
{
    nimble_port_run();
}

// Preparación inicial del motor Bluetooth
void initialize_bluetooth(void)
{
    nimble_port_init();
    ble_svc_gap_device_name_set("ESP32S3_PRESION"); // Nombre visible en el escáner
    // ble_svc_gap_device_name_set("ESP32S3_V2");
    ble_svc_gap_init();
    ble_svc_gatt_init();
    
    // Registramos nuestros servicios GATT en la pila
    ble_gatts_count_cfg(gatt_svcs);
    ble_gatts_add_svcs(gatt_svcs);
    
    // Establecemos el tamaño preferido del MTU interno a 512 bytes. 
    // Esencial para que nuestro ensamblaje en gatt_ota_data_access no se desborde.
    ble_att_set_preferred_mtu(512);
    
    // Establecemos el callback de inicio y lanzamos NimBLE en el Scheduler.
    ble_hs_cfg.sync_cb = ble_app_on_sync;
    nimble_port_freertos_init(host_task);
}

// Función que manda 4 bytes del sensor a Angular.
void send_ble_data(float value)
{
    if (ble_connection_handle != 0xFFFF)
    {
        // Mete el float en un mbuf y lo envía como Notificación (modo de empuje de servidor).
        struct os_mbuf *om = ble_hs_mbuf_from_flat(&value, sizeof(value));
        ble_gatts_notify_custom(ble_connection_handle, gatt_pressure_handle, om);
    }
}

// Marcador lógico de finalización para la interfaz en Angular
void send_ble_end_marker(void)
{
    float end_marker = -9999.0;
    send_ble_data(end_marker);
}