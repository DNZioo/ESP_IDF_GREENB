#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/sys.h"
#include "esp_log.h"
#include "driver/gpio.h"

#define WIFI_SSID "ESP32"
#define WIFI_PASS "11112222"
#define TCP_SERVER_IP "192.168.4.1"
#define TCP_PORT 8888
#define SLAVE_ID 0x01 // Unique ID for this slave


static const char *TAG = "TCP_SLAVE";


static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WIFI station started. Attempting to connect to AP....");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGE(TAG, "WIFI disconnected. Retrying connection....");
        esp_wifi_connect();
        ESP_LOGW(TAG, "retry to connect to the AP");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Successfully connected to WIFI. Got ip:%s", ip4addr_ntoa(&event->ip_info.ip));
    }
}

void wifi_init_sta() {
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT,IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_LR));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

typedef struct {
    uint8_t address;         // Target device address (1 byte)
    uint8_t function_code;   // Function code (1 byte)
    uint16_t data_len;       // Length of the payload (2 bytes)
    uint8_t data[256];       // Payload data (variable length, up to 256 bytes)
    uint16_t checksum;       // Error-checking mechanism (2 bytes)
} frame_t;

// Calculate checksum
uint16_t calculate_checksum(const uint8_t *data, size_t len) {
    uint16_t checksum = 0;
    for (size_t i = 0; i < len; i++) {
        checksum += data[i];
    }
    return checksum;
}

// // Deserialize frame from raw bytes
// void deserialize_frame(frame_t *frame, const uint8_t *buffer) {
//     frame->address = buffer[0];
//     frame->function_code = buffer[1];
//     frame->data_len = ((uint16_t)buffer[2] << 8) | buffer[3]; // Big-endian

//     memcpy(frame->data, &buffer[4], frame->data_len);

//     uint16_t checksum = ((uint16_t)buffer[4 + frame->data_len] << 8) | buffer[5 + frame->data_len]; // Big-endian
//     frame->checksum = checksum;
// }

void deserialize_frame(frame_t *frame, const uint8_t *buffer) {
    frame->address = buffer[0];
    frame->function_code = buffer[1];
    frame->data_len = ((uint16_t)buffer[2] << 8) | buffer[3]; // Big-endian

    memcpy(frame->data, &buffer[4], frame->data_len);

    uint16_t checksum = ((uint16_t)buffer[4 + frame->data_len] << 8) | buffer[5 + frame->data_len]; // Big-endian
    frame->checksum = checksum;

    // Validate address
    if (frame->address != SLAVE_ID) {
        ESP_LOGW(TAG, "Frame rejected: Address mismatch (Expected: %d, Received: %d)", SLAVE_ID, frame->address);
        memset(frame, 0, sizeof(frame_t)); // Clear the frame to indicate rejection
    }
}

static void tcp_task(void *pvParameters) {
    struct sockaddr_in destAddr;
    destAddr.sin_addr.s_addr = inet_addr(TCP_SERVER_IP);
    destAddr.sin_family = AF_INET;
    destAddr.sin_port = htons(TCP_PORT);

    int sock = -1; // Initialize socket to -1 to indicate no active connection
    bool connected = false;

    while (1) {
        if (!connected) {
            // Attempt to create a new socket
            sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
            if (sock < 0) {
                ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
                vTaskDelay(2000 / portTICK_PERIOD_MS); // Wait before retrying
                continue;
            }

            // Attempt to connect to the server
            int err = connect(sock, (struct sockaddr *)&destAddr, sizeof(destAddr));
            if (err != 0) {
                ESP_LOGE(TAG, "Socket unable to connect: errno %d", errno);
                close(sock); // Close the socket on failure
                vTaskDelay(2000 / portTICK_PERIOD_MS); // Wait before retrying
                continue;
            }
            ESP_LOGI(TAG, "Successfully connected to TCP server");
            connected = true;
        }

        // Receive the frame
        uint8_t buffer[264]; // Max frame size: 1 + 1 + 2 + 256 + 2
        int bytes_received = recv(sock, buffer, sizeof(buffer), 0);
        if (bytes_received > 0) {
            frame_t frame;
            deserialize_frame(&frame, buffer);

            // Check if the frame was rejected due to address mismatch
            if (frame.address == 0) {
                ESP_LOGW(TAG, "Frame rejected by deserializer.");
                continue; // Skip further processing
            }

            // Verify checksum
            uint16_t calculated_checksum = calculate_checksum(buffer, bytes_received - 2); // Exclude checksum field
            if (calculated_checksum != frame.checksum) {
                ESP_LOGE(TAG, "Checksum mismatch! Frame corrupted.");
            } else {
                ESP_LOGI(TAG, "Received valid frame: Address=%d, Function=%d, Data=%.*s",
                         frame.address, frame.function_code, frame.data_len, frame.data);
            }
        } else if (bytes_received == 0) {
            ESP_LOGW(TAG, "Server closed the connection");
            close(sock);
            connected = false;
        } else {
            ESP_LOGE(TAG, "Error receiving data: errno %d", errno);
            close(sock);
            connected = false;
        }

        vTaskDelay(5000 / portTICK_PERIOD_MS);
    }
}

// static void tcp_task(void *pvParameters) {
//     struct sockaddr_in destAddr;
//     destAddr.sin_addr.s_addr = inet_addr(TCP_SERVER_IP);
//     destAddr.sin_family = AF_INET;
//     destAddr.sin_port = htons(TCP_PORT);

//     int sock = -1; // Initialize socket to -1 to indicate no active connection
//     bool connected = false;

//     while (1) {
//         if (!connected) {
//             // Attempt to create a new socket
//             sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
//             if (sock < 0) {
//                 ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
//                 vTaskDelay(2000 / portTICK_PERIOD_MS); // Wait before retrying
//                 continue;
//             }

//             // Attempt to connect to the server
//             int err = connect(sock, (struct sockaddr *)&destAddr, sizeof(destAddr));
//             if (err != 0) {
//                 ESP_LOGE(TAG, "Socket unable to connect: errno %d", errno);
//                 close(sock); // Close the socket on failure
//                 vTaskDelay(2000 / portTICK_PERIOD_MS); // Wait before retrying
//                 continue;
//             }
//             ESP_LOGI(TAG, "Successfully connected to TCP server");
//             connected = true;
//         }
//         // Receive the frame
//         uint8_t buffer[264]; // Max frame size: 1 + 1 + 2 + 256 + 2
//         int bytes_received = recv(sock, buffer, sizeof(buffer), 0);
//         if (bytes_received > 0) {
//             frame_t frame;
//             deserialize_frame(&frame, buffer);

//             // Verify checksum
//             uint16_t calculated_checksum = calculate_checksum(buffer, bytes_received - 2); // Exclude checksum field
//             if (calculated_checksum != frame.checksum) {
//                 ESP_LOGE(TAG, "Checksum mismatch! Frame corrupted.");
//             } else {
//                 ESP_LOGI(TAG, "Received frame: Address=%d, Function=%d, Data=%.*s",
//                         frame.address, frame.function_code, frame.data_len, frame.data);
//             }
//         } else if (bytes_received == 0) {
//             ESP_LOGW(TAG, "Server closed the connection");
//             close(sock);
//             connected = false;
//         } else {
//             ESP_LOGE(TAG, "Error receiving data: errno %d", errno);
//             close(sock);
//             connected = false;
//         }

//         vTaskDelay(5000 / portTICK_PERIOD_MS);
//     }
// }


void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret); 
    wifi_init_sta();

    // ESP_LOGI(TAG, "WiFi setup complete");

    ESP_LOGI(TAG, "Starting TCP server task");
    xTaskCreate(tcp_task, "tcp_server", 4096, NULL, 5, NULL);
}