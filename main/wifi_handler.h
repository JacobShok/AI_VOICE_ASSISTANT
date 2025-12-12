#ifndef WIFI_HANDLER_H
#define WIFI_HANDLER_H

#include "esp_err.h"

/**
 * @brief Initialize WiFi and connect to network
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t wifi_connect_init(void);

/**
 * @brief Check if WiFi is connected
 * 
 * @return true if connected, false otherwise
 */
bool wifi_is_connected(void);

#endif // WIFI_HANDLER_H