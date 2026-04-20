idf_component_register(
    SRCS "main.c" 
         "sources/spiffs_manager.c" 
         "sources/pressure_sensor.c" 
         "sources/bluetooth_manager.c"
    INCLUDE_DIRS "." "header" 
    PRIV_REQUIRES json esp_adc spiffs nvs_flash bt
)