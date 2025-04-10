#include "nvs_storage.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include <string.h>

#define TAG_NVS "NVS_STORAGE"

// Function to initialize NVS
void initialize_nvs() {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}

// Function to save previous IP to NVS
void save_previous_ip(const char *prev_ip) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_NVS, "Error opening NVS: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_set_str(nvs_handle, "prev_ip", prev_ip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_NVS, "Error saving prev_ip: %s", esp_err_to_name(err));
    }

    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_NVS, "Error committing changes: %s", esp_err_to_name(err));
    }

    nvs_close(nvs_handle);
}

// Function to save pump IP to NVS
void save_pump_ip(const char *pump_ip) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_NVS, "Error opening NVS: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_set_str(nvs_handle, "last_pump", pump_ip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_NVS, "Error saving last_pump: %s", esp_err_to_name(err));
    }

    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_NVS, "Error committing changes: %s", esp_err_to_name(err));
    }

    nvs_close(nvs_handle);
}

// Function to load IP addresses from NVS
void load_ip_addresses(char *prev_ip, size_t prev_ip_size, 
                      char *last_pump_ip, size_t last_pump_size) {
    if (!prev_ip || !last_pump_ip) {
        ESP_LOGE(TAG_NVS, "Invalid buffer pointers");
        return;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_NVS, "Error opening NVS: %s", esp_err_to_name(err));
        prev_ip[0] = '\0';
        last_pump_ip[0] = '\0';
        return;
    }

    // Load previous IP
    err = nvs_get_str(nvs_handle, "prev_ip", prev_ip, &prev_ip_size);
    if (err != ESP_OK) {
        ESP_LOGW(TAG_NVS, "prev_ip not found, setting empty");
        prev_ip[0] = '\0';
    }

    // Load pump IP
    err = nvs_get_str(nvs_handle, "last_pump", last_pump_ip, &last_pump_size);
    if (err != ESP_OK) {
        ESP_LOGW(TAG_NVS, "last_pump not found, setting empty");
        last_pump_ip[0] = '\0';
    }

    nvs_close(nvs_handle);
}