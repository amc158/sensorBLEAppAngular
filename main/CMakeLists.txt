idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES json esp_adc spiffs nvs_flash bt)