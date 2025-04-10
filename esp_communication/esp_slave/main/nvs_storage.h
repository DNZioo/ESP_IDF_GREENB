#ifndef NVS_STORAGE_H
#define NVS_STORAGE_H

#include "esp_err.h"

// NVS Namespace and Keys
#define NVS_NAMESPACE "ip_storage"
#define PREVIOUS_IP_KEY "previous_ip"
#define LAST_PUMP_IP_KEY "last_pump_ip"

// Function prototypes
void initialize_nvs();
void save_previous_ip(const char *prev_ip);
void save_pump_ip(const char *pump_ip);
void load_ip_addresses(char *previous_ip, size_t prev_ip_len, char *last_pump_ip, size_t last_ip_len);

#endif // NVS_STORAGE_H
