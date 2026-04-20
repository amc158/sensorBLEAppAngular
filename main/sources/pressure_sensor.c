#include "pressure_sensor.h"
#include "esp_adc/adc_oneshot.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Definimos el pin del ADC donde esta conectado el sensor
#define SENSOR_ADC_CHANNEL ADC_CHANNEL_3 // Corresponde al GPIO 4 en el ESP32-S3

// Variables internas del sensor
static adc_oneshot_unit_handle_t adc1_handle;
static float dynamic_zero_voltage = 0.502;
static float filtered_pressure = 0.0;
static const float alpha = 0.15; // Factor de suavizado para el filtro (EMA)

void initialize_sensor(void) {
    // 1. Inicializamos la unidad ADC 1
    adc_oneshot_unit_init_cfg_t init_cfg = { .unit_id = ADC_UNIT_1 };
    adc_oneshot_new_unit(&init_cfg, &adc1_handle);
    
    // 2. Configuramos el canal especifico del sensor con maxima atenuacion (para leer hasta ~3.1V)
    adc_oneshot_chan_cfg_t adc_config = { .bitwidth = ADC_BITWIDTH_DEFAULT, .atten = ADC_ATTEN_DB_12 };
    adc_oneshot_config_channel(adc1_handle, SENSOR_ADC_CHANNEL, &adc_config);

    // 3. Calibracion de arranque: Tomamos 40 muestras para calcular el voltaje "cero" actual
    float sum_voltage = 0;
    for(int i = 0; i < 40; i++) {
        int raw_value; 
        adc_oneshot_read(adc1_handle, SENSOR_ADC_CHANNEL, &raw_value);
        sum_voltage += (raw_value / 4095.0) * 3.3 * 1.5;
        vTaskDelay(pdMS_TO_TICKS(25)); // Esperamos 25ms entre cada lectura
    }
    
    // Establecemos la media como nuestro voltaje de referencia en reposo
    dynamic_zero_voltage = sum_voltage / 40.0;
}

float read_pressure(void) {
    // 1. Leemos el valor bruto del ADC (0 a 4095)
    int raw_value;
    adc_oneshot_read(adc1_handle, SENSOR_ADC_CHANNEL, &raw_value);
    
    // 2. Convertimos el valor bruto a voltaje real y aplicamos factor de correccion
    float voltage = ((raw_value / 4095.0) * 3.3) * 1.5;
    
    // 3. Calculamos la presion instantanea comparando con nuestro cero calibrado
    float instantaneous_pressure = (voltage - dynamic_zero_voltage) * -250.0;
    
    // 4. Aplicamos el filtro EMA (Exponential Moving Average) para quitar ruido electrico
    filtered_pressure = (alpha * instantaneous_pressure) + ((1.0 - alpha) * filtered_pressure);
    
    return filtered_pressure;
}