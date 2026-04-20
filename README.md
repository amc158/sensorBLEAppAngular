# 📡 Firmware ESP32-S3: Sensor de Presión BLE (v2 - Arquitectura Modular)

Este repositorio contiene el firmware (código C/ESP-IDF) para un microcontrolador ESP32-S3. Su función principal es leer un sensor de presión analógico, procesar los datos y transmitirlos de forma inalámbrica.

Esta versión evoluciona desde la estructura monolítica inicial (v1) hacia un **código modular seccionado en librerías**. Esto separa completamente la lógica en archivos de cabecera (`.h`) y archivos fuente (`.c`), facilitando enormemente el mantenimiento, la lectura y la escalabilidad del sistema.

## 🏗️ Arquitectura Modular (Novedades)

El código principal del programa (antes concentrado en `main.c`) ahora se divide en tres módulos independientes alojados en las carpetas `header/` y `sources/`:

* 💾 **`spiffs_manager`**: Gestiona el sistema de archivos físicos, permitiendo abrir, inicializar, guardar y borrar historiales en formato JSON.
* 📊 **`pressure_sensor`**: Configura la lectura analógica (ADC), calibra el voltaje "cero" de arranque y aplica el filtro matemático de suavizado.
* 📶 **`bluetooth_manager`**: Controla la pila NimBLE, gestionando las conexiones, el servidor GATT, las notificaciones y la recepción de comandos en tiempo real.

## ✨ Características Principales

* **Lectura de Sensor (ADC):** Lee el voltaje de un sensor de presión por el pin GPIO 4 a una frecuencia de 10Hz (10 veces por segundo).
* **Calibración Dinámica:** Al arrancar, el chip calcula automáticamente el voltaje "cero" durante los primeros milisegundos.
* **Transmisión Bluetooth LE (NimBLE):** Actúa como un servidor GATT enviando notificaciones constantes con la presión actual en milibares (mB).
* **Almacenamiento Interno (SPIFFS "Caja Negra"):** Cada 2 segundos, el ESP32 calcula la media matemática de las últimas 20 lecturas y la guarda de forma segura en un archivo `data.json` dentro de su memoria Flash.
* **Sistema de Comandos:** Acepta órdenes por Bluetooth para interactuar con él:
  * `0`: Apagar envíos en vivo.
  * `1`: Activar envíos en vivo.
  * `2`: Descargar registro histórico (SPIFFS).
  * `3`: Borrar memoria interna.

## 📱 Integración con la App Cliente

⚠️ **IMPORTANTE:** Este proyecto es solo el "cerebro" (Hardware). Está diseñado para trabajar en conjunto con el repositorio hermano:

🔗 **Repositorio de la App:** [https://github.com/ualamc158/app-sensor-ble](https://github.com/ualamc158/app-sensor-ble)

**app-sensor-ble** es la aplicación cliente desarrollada con Angular y Capacitor. Esa aplicación es la encargada de conectarse a este ESP32 y permite:
1. **Controlar el chip** (enviar los comandos de Activar/Pausar).
2. **Visualizar** la cascada de datos en tiempo real.
3. **Descargar y exportar** el archivo de la memoria SPIFFS a un formato de tabla legible.
4. Funcionar de forma nativa tanto en el navegador web (**Chrome vía Web Bluetooth**) como instalada como aplicación en **Android**.

## 🛠️ Entorno de Desarrollo
* Framework: ESP-IDF v5.3.4
* Compilador: Xtensa ESP32-S3 Toolchain
* Componentes principales: `nvs_flash`, `spiffs`, `esp_adc`, `nimble`, `cJSON`.