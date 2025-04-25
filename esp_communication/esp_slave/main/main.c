#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/sys.h"
#include "esp_log.h"


#define WIFI_SSID "ESP32"
#define WIFI_PASS "11112222"
#define DISCOVERY_PORT 8888
#define TCP_PORT 5555
#define RESPONSE_DELAY_MS 5000  
#define IP_CHANGED_BIT BIT0
#define NET_HOUSE_IP "01"

static EventGroupHandle_t wifi_event_group;
static char current_ip  [16] = {0};
static char previous_ip [16] = {0};
static char last_Pump_IP[16] = {0};


static const char *TAG = "ESP_NETHouse";

#pragma pack(push, 1)  // Ensure no padding between struct members
typedef struct {
    uint8_t id;       // Device ID (e.g., 01 for nethouse)
    uint8_t command;  // Command byte (e.g., 0x00, 0x01, etc.)
    uint8_t crc;      // CRC-8 checksum (computed over id + command)
} frame_t;
#pragma pack(pop)      // Restore default struct alignment

// ============================ NVS Utility ============================

void load_previous_ip_from_nvs(char *buffer, size_t len) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("storage", NVS_READONLY, &handle);
    if (err == ESP_OK) {
        err = nvs_get_str(handle, "last_ip", buffer, &len);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Loaded previous IP from NVS: %s", buffer);
        } else {
            ESP_LOGW(TAG, "No previous IP stored in NVS.");
        }
        nvs_close(handle);
    } else {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
    }
}

void save_current_ip_to_nvs(const char *ip) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        nvs_set_str(handle, "last_ip", ip);
        nvs_commit(handle);
        nvs_close(handle);
        ESP_LOGI(TAG, "Saved current IP to NVS: %s", ip);
    } else {
        ESP_LOGE(TAG, "Failed to write to NVS: %s", esp_err_to_name(err));
    }
}

// Add to NVS Utility section
void save_pump_ip_to_nvs(const char *ip) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        nvs_set_str(handle, "last_pump_ip", ip);
        nvs_commit(handle);
        nvs_close(handle);
        ESP_LOGI(TAG, "Saved pump IP to NVS: %s", ip);
    }
}

void load_pump_ip_from_nvs(char *buffer, size_t len) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("storage", NVS_READONLY, &handle);
    if (err == ESP_OK) {
        err = nvs_get_str(handle, "last_pump_ip", buffer, &len);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Loaded pump IP from NVS: %s", buffer);
        }
        nvs_close(handle);
    }
}

// ============================ Wi-Fi Events ============================

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        ESP_LOGE(TAG, "Retry to connect to the AP");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        strncpy(current_ip, ip4addr_ntoa(&event->ip_info.ip), sizeof(current_ip) - 1);
        current_ip[sizeof(current_ip) - 1] = '\0';

        load_previous_ip_from_nvs(previous_ip, sizeof(previous_ip));
        save_current_ip_to_nvs(current_ip);
        printf("Current IP: %s || Previous IP: %s\n", current_ip, previous_ip);
        if (strcmp(current_ip, previous_ip) != 0) {
            xEventGroupSetBits(wifi_event_group, IP_CHANGED_BIT);
            ESP_LOGI(TAG, "IP changed: %s to %s", previous_ip, current_ip);
        }
    }
}
void wifi_init_sta() {
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL,NULL);
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK,
        },
    };
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config);
    esp_wifi_start();
}

// ============================ UDP Responder ============================

void udp_responder_task(void *pvParameters) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);

    // Configure broadcast socket
    int broadcast_enable = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast_enable, sizeof(broadcast_enable));

    // Setup listening address
    struct sockaddr_in recv_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DISCOVERY_PORT),
        .sin_addr.s_addr = INADDR_ANY
    };

    bind(sock, (struct sockaddr *)&recv_addr, sizeof(recv_addr));
    // Set receive timeout
    struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  
    bool responded_to_pump = false;

    while (1) {

        if (xEventGroupWaitBits(wifi_event_group, IP_CHANGED_BIT, pdTRUE, pdFALSE, 0)) {
            responded_to_pump = false;
        }

        struct sockaddr_in pump_addr;
        socklen_t addr_len = sizeof(pump_addr);
        char buffer[64];

        int len = recvfrom(sock, buffer, sizeof(buffer) - 1, 0, (struct sockaddr *)&pump_addr, &addr_len);
        if (len > 0) {
            buffer[len] = '\0';
                const char *pump_ip = inet_ntoa(pump_addr.sin_addr);
                ESP_LOGI(TAG, "Received: %s", pump_ip);
                strcpy(last_Pump_IP, pump_ip);
                save_pump_ip_to_nvs(last_Pump_IP);
                responded_to_pump = false;  // reset flag for new pump IP
        }

        if (!responded_to_pump && strlen(last_Pump_IP) > 0) {
            struct sockaddr_in reply_addr = {
                .sin_family = AF_INET,
                .sin_port = htons(DISCOVERY_PORT),
                .sin_addr.s_addr = inet_addr(last_Pump_IP),
            };

            char reply_msg[64];
            snprintf(reply_msg, sizeof(reply_msg), "%s:%s", NET_HOUSE_IP, current_ip);

            int sent = sendto(sock, reply_msg, strlen(reply_msg), 0, (struct sockaddr *)&reply_addr, sizeof(reply_addr));
            if (sent > 0) {
                ESP_LOGI(TAG, "Respond: %s", reply_msg);
                responded_to_pump = true;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    close(sock);
    vTaskDelete(NULL);
}

// ============================ CRC Computation =============================
uint8_t crc8_compute(const uint8_t *data, size_t len) {
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x80)
                crc = (crc << 1) ^ 0x07;
            else
                crc <<= 1;
        }
    }
    return crc;
}

// Append CRC to a message
size_t crc8_append(uint8_t *data, size_t len) {
    uint8_t crc = crc8_compute(data, len);
    data[len] = crc;  // Append CRC at the end
    return len + 1;   // Return new length
}

// Verify that received buffer has correct CRC
bool crc8_verify(const uint8_t *data, size_t len_with_crc) {
    uint8_t remainder = crc8_compute(data, len_with_crc);
    ESP_LOGI(TAG, "Verify CRC: 0x%02X", remainder);
    return remainder == 0;
}
// ============================ TCP Task Function ============================

void tcp_server_task(void *pvParameters) {
    const char *payload = "Hello PUMP!";
    struct sockaddr_in destAddr;
    destAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    destAddr.sin_family = AF_INET;
    destAddr.sin_port = htons(TCP_PORT);

    // Create a socket
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    // Bind the socket
    int err = bind(sock, (struct sockaddr *)&destAddr, sizeof(destAddr));
    if (err != 0) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    // Listen for incoming connections
    err = listen(sock, 1);
    if (err != 0) {
        ESP_LOGE(TAG, "Error occurred during listen: errno %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Socket listening");

    while (1) {
        // Accept a connection
        struct sockaddr_in sourceAddr;
        uint addrLen = sizeof(sourceAddr);
        int clientSock = accept(sock, (struct sockaddr *)&sourceAddr, &addrLen);
        if (clientSock < 0) {
            ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
            continue;
        }

        // Set a timeout for recv()
        struct timeval timeout = { .tv_sec = 5, .tv_usec = 0 }; // 5-second timeout
        setsockopt(clientSock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        // Receive data from the client
        while (1) {
            uint8_t buffer[256];
            int len = recv(clientSock, buffer, sizeof(buffer) - 1, 0);
            
            if (len < 0) {
                ESP_LOGE(TAG, "recv failed: errno %d", errno);
                close(clientSock);
                break;
            } else if (len == 0) {
                ESP_LOGI(TAG, "Connection closed");
                close(clientSock);
                break;
            } else if (len >= 1) {
                uint8_t crc_byte = buffer[len - 1];
                size_t data_len = len - 1;
            
                ESP_LOGI(TAG, "Received: %.*s 0x%02X", data_len, buffer, crc_byte);  // Log message and CRC
            
                // Verify CRC once
                if (crc8_verify(buffer, len)) {
                    ESP_LOGI(TAG, "CRC8 verified successfully.");
                    buffer[data_len] = '\0';  // Null-terminate after CRC check
            
                    // Prepare a response with CRC
                    const char *response = "Hello PUMP";
                    size_t response_len = strlen(response);
                    uint8_t output_buffer[response_len + 1]; 
                    memcpy(output_buffer, response, response_len);
                    size_t total_len = crc8_append(output_buffer, response_len);
            
                    int sent = send(clientSock, output_buffer, total_len, 0);
                    if (sent < 0) {
                        ESP_LOGE(TAG, "Error sending response: errno %d", errno);
                    } else {
                        ESP_LOGI(TAG, "Send: %s 0x%02X", response, output_buffer[total_len - 1]);
                    }
            
                } else {
                    ESP_LOGE(TAG, "CRC8 verification failed.");
                }
            } else {
                ESP_LOGW(TAG, "Received empty message.");
            }                     
        }
    }
}

void app_main() {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret); 
    wifi_event_group = xEventGroupCreate();
    xEventGroupClearBits(wifi_event_group, IP_CHANGED_BIT);  // Clear any stale bits
    load_pump_ip_from_nvs(last_Pump_IP, sizeof(last_Pump_IP));
    printf("Last Pump IP: %s\n", last_Pump_IP);  

    wifi_init_sta();
    xTaskCreate(udp_responder_task, "udp_responder", 8192, NULL, 5, NULL);  // Increased stack size
    xTaskCreate(tcp_server_task, "tcp_server", 8192, NULL, 5, NULL);  // Increased stack size
}