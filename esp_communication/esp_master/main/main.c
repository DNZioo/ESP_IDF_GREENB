#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/sys.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_crc.h"


#define WIFI_SSID "ESP32"
#define WIFI_PASS "11112222"
#define PORT 8888
#define TCP_PORT 5555
#define BROADCAST_IP "192.168.4.255"
#define MAX_IP_LENGTH   16
#define DISCOVERY_MSG_SIZE 64
#define WIFI_CONNECTED_BIT BIT0

#define ID_LENGTH 16
#define IP_LENGTH 16
#define MAX_nethouse 5

int sock = -1;

static const char *TAG = "ESP_PUMP";

static EventGroupHandle_t wifi_event_group;
static char current_ip[MAX_IP_LENGTH] = {0}; 
static char previous_ip[MAX_IP_LENGTH] = {0}; 


// ============================ Handle Net Houses ============================

typedef struct {
    char id[ID_LENGTH]; 
    char ip[IP_LENGTH];
    bool active;  // to mark if this nethouse is currently responding
    uint32_t last_seen; // timestamp of last response
} nethouse_t;

static nethouse_t nethouse[MAX_nethouse] = {
    {"01", "", false, 0}, // Initialize with empty strings and default values
    {"02", "", false, 0},
    {"03", "", false, 0},
    {"04", "", false, 0},
    {"05", "", false, 0}
};
static int  num_nethouse = MAX_nethouse; // Number of nethouse currently known

void handle_nethouse_response(const char* message, const char* source_ip) {
    // Find the colon separator
    char* colon = strchr(message, ':');
    if (!colon) return;  // Invalid format
    
    // Extract ID (before colon)
    char id[ID_LENGTH];
    int id_length = colon - message;
    if (id_length >= ID_LENGTH) id_length = ID_LENGTH - 1;
    strncpy(id, message, id_length);
    id[id_length] = '\0';
    
    // Find this nethouse in our array
    for (int i = 0; i < num_nethouse; i++) {
        if (strcmp(nethouse[i].id, id) == 0) {
            // Update existing entry
            strncpy(nethouse[i].ip, source_ip, IP_LENGTH - 1);
            nethouse[i].active = true;
            nethouse[i].last_seen = xTaskGetTickCount();
            
            // ESP_LOGI(TAG, "NetHouse %s:%s", id, source_ip);
            return;
        }
    }
    ESP_LOGW(TAG, "Received response from unknown NetHouse ID: %s", id);
}

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

// ============================ Wi-Fi Events ============================


static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
        ESP_LOGE(TAG, "Retry to connect to the AP");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        strncpy(current_ip, ip4addr_ntoa(&event->ip_info.ip), MAX_IP_LENGTH - 1);
        // current_ip[sizeof(current_ip) - 1] = '\0';

        //load and save IP addresses
        load_previous_ip_from_nvs(previous_ip, sizeof(previous_ip));
        save_current_ip_to_nvs(current_ip);
        ESP_LOGI(TAG, "IP changed: %s to %s", current_ip, previous_ip);
        printf("Current IP: %s || Previous IP: %s\n", current_ip, previous_ip);

        //Signal that WIFI is connected
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        
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
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config);
    esp_wifi_start();
}

// ============================ UDP Broadcast ============================

void udp_broadcast_task(void *pvParameters) {
    int sock = sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);

    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create socket: errno %d", errno);
        vTaskDelete(NULL);
    }
    int broadcast_enable = 1;  //enable broadcast
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast_enable, sizeof(broadcast_enable));

    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,     //IPv4
        .sin_port = htons(PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY) //Bind to all interfaces
    };
    bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr));

    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in broadcast_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(PORT),
        .sin_addr.s_addr = inet_addr(BROADCAST_IP)
    };

    bool nethouse_responded = false;
    // char last_sent_ip[MAX_IP_LENGTH] = {0}; // Store the last sent IP address
    
    while (1) {

    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, false, true, portMAX_DELAY);  

        if (strcmp(previous_ip, current_ip) != 0 || !nethouse_responded) {
            sendto(sock, current_ip, strlen(current_ip), 0, (struct sockaddr *)&broadcast_addr, sizeof(broadcast_addr));
            strncpy(previous_ip, current_ip, sizeof(previous_ip) - 1);
            ESP_LOGI(TAG, "Broadcasting: %s", current_ip);
        }
        
        struct sockaddr_in slave_addr;
        socklen_t addr_len = sizeof(slave_addr);
        char response[64];

        int received = recvfrom(sock, response, sizeof(response) - 1, 0, (struct sockaddr *)&slave_addr, &addr_len);
        if (received > 0) {
            response[received] = '\0';
            // Handle the response
            handle_nethouse_response(response, inet_ntoa(slave_addr.sin_addr)); //ip from socket
            ESP_LOGI(TAG, "%s:%s",nethouse[0].id, nethouse[0].ip);
            ESP_LOGI(TAG, "%s:%s",nethouse[1].id, nethouse[1].ip);
            ESP_LOGI(TAG, "%s:%s",nethouse[2].id, nethouse[2].ip);
            ESP_LOGI(TAG, "%s:%s",nethouse[3].id, nethouse[3].ip);
            ESP_LOGI(TAG, "%s:%s",nethouse[4].id, nethouse[4].ip);
            nethouse_responded = true;
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
        close(sock);
    vTaskDelete(NULL);
}

// ============================ CRC Computation ============================

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

// ============================ TCP Task ============================

static void tcp_client_task(void *pvParameters) {
    const char *tcp_payload = "Hello nethouse!";
    int sock = -1;
    bool connected = false;

    while (1) {
        // Wait until we have a valid NetHouse IP
        if (strlen(nethouse[0].ip) == 0) {
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            continue;
        }

        if (!connected) {
            // Create a socket
            sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
            if (sock < 0) {
                ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
                vTaskDelay(2000 / portTICK_PERIOD_MS);
                continue;
            }

            struct sockaddr_in destAddr;
            destAddr.sin_addr.s_addr = inet_addr(nethouse[0].ip);
            destAddr.sin_family = AF_INET;
            destAddr.sin_port = htons(TCP_PORT);

            // Connect to the server
            int err = connect(sock, (struct sockaddr *)&destAddr, sizeof(destAddr));
            if (err != 0) {
                ESP_LOGE(TAG, "Socket unable to connect: errno %d", errno);
                close(sock);
                vTaskDelay(2000 / portTICK_PERIOD_MS);
                continue;
            }
            connected = true;
        }

        // Prepare the message with CRC8
        size_t payload_len = strlen(tcp_payload);
        uint8_t output_buffer[payload_len + 1];
        memcpy(output_buffer, tcp_payload, payload_len);
        size_t total_len = crc8_append(output_buffer, payload_len);

        // Send the message with CRC
        int err = send(sock, output_buffer, total_len, 0);
        ESP_LOGI(TAG, "Sent: %s 0x%02X", tcp_payload, output_buffer[total_len - 1]);
        if (err < 0) {
            ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
            close(sock);
            connected = false;
            vTaskDelay(2000 / portTICK_PERIOD_MS);
            continue;
        }

        // Wait for a response from the server
        uint8_t buffer[256];
        int len = recv(sock, buffer, sizeof(buffer) - 1, 0);
        if (len > 0) {
            if (len >= 1) {
                uint8_t crc_byte = buffer[len - 1];
                size_t data_len = len - 1;

                ESP_LOGI(TAG, "Received: %.*s 0x%02X", data_len, buffer, crc_byte);  // Logging raw data + CRC

                if (crc8_verify(buffer, len)) {
                    ESP_LOGI(TAG, "CRC8 verified successfully.");
                    buffer[data_len] = '\0';  // Null-terminate *after* CRC check
                } else {
                    ESP_LOGE(TAG, "CRC8 verification failed.");
                }

            } else {
                ESP_LOGW(TAG, "Received empty message.");
            }
        }
        vTaskDelay(2000 / portTICK_PERIOD_MS);
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

    wifi_init_sta();
    xTaskCreate(udp_broadcast_task, "udp_server", 4096, NULL, 5, NULL);
    xTaskCreate(tcp_client_task, "tcp_client", 4096, NULL, 5, NULL);
}