#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"
#include "esp_netif.h"

#define WIFI_SSID "ESP32"
#define WIFI_PASS "11112222"
#define WIFI_SSID_ROUTER "DNZio"
#define WIFI_PASS_ROUTER "11112222"
#define PORT 8888
static const char *TAG = "MASTER";
static int sock;

static EventGroupHandle_t wifi_event_group;
const int CONNECTED_BIT = BIT0;

typedef struct {
    uint8_t address;         // Target device address (1 byte)
    uint8_t state;           // State (1 byte)
    uint16_t checksum;       // Error-checking mechanism (2 bytes)
} modbus_frame_t;

uint16_t calculate_checksum(uint8_t id, uint8_t state) {
    uint16_t sum = (uint16_t)id + (uint16_t)state; // Ensure 16-bit addition
    return ~sum; // 1's complement
}

// Verify checksum using your specified method
int validate_frame(const uint8_t *buffer) {
    // Extract 8-bit fields
    uint8_t id = buffer[0];
    uint8_t state = buffer[1];
    
    // Reconstruct 16-bit checksum (little-endian)
    uint16_t received_checksum = (buffer[3] << 8) | buffer[2];
    
    // Calculate verification sum (16-bit arithmetic)
    uint16_t data_sum = (uint16_t)id + (uint16_t)state;
    uint16_t verification_sum = data_sum + received_checksum;
    uint16_t result = ~verification_sum;
    
    // Debug output
    ESP_LOGI(TAG, "Validation: ID = %d State = %d Checksum = %d", id, state, result);
    return (result == 0) ? 0 : -1;
}

// Modify the deserialize_frame function:
int deserialize_frame(modbus_frame_t *frame, const uint8_t *buffer) {
    frame->address = buffer[0];
    frame->state = buffer[1];
    frame->checksum = ((uint16_t)buffer[3] << 8) | buffer[2];
    
    return validate_frame(buffer);
}

void tcp_task(void *pvParameters) {
    struct sockaddr_in destAddr;
    destAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    destAddr.sin_family = AF_INET;
    destAddr.sin_port = htons(PORT);

    // Create a socket
    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
        // return;
    }

    // Bind the socket
    int err = bind(sock, (struct sockaddr *)&destAddr, sizeof(destAddr));
    if (err != 0) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        close(sock);
        vTaskDelete(NULL);
        // return;
    }
    ESP_LOGI(TAG, "Socket bound successfully to port %d.", PORT);

    // Listen for incoming connections
    err = listen(sock, 1);
    if (err != 0) {
        ESP_LOGE(TAG, "Error occurred during listen: errno %d", errno);
        close(sock);
        vTaskDelete(NULL);
        // return;
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

        char clientIP[16];
        inet_ntoa_r(sourceAddr.sin_addr, clientIP, sizeof(clientIP));
        ESP_LOGI(TAG, "Client connected from IP: %s, port: %d", clientIP, ntohs(sourceAddr.sin_port));
        // Set a timeout for recv()
        struct timeval timeout = { .tv_sec = 10, .tv_usec = 0 }; // 5-second timeout
        setsockopt(clientSock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        while (1){
            uint8_t buffer[4]; // Exactly 4 bytes expected
            int bytes_received = recv(clientSock, buffer, sizeof(buffer), 0);
            
            if (bytes_received == 4) {
                modbus_frame_t frame;
                if (deserialize_frame(&frame, buffer) == 0) {
                    // ESP_LOGI(TAG, "Valid frame - Address=%d, State=%d, checksum=%d",
                    //          frame.address, frame.state, frame.checksum);
                } else {
                    ESP_LOGE(TAG, "Checksum mismatch! Frame corrupted.");
                }
            } else if (bytes_received < 0) {
                ESP_LOGE(TAG, "Error receiving data: errno %d", errno);
                close(clientSock);
                break;
            }
            vTaskDelay(10 / portTICK_PERIOD_MS); 
        }   
    }
}

// WiFi Event Handler
static esp_err_t event_handler(void *ctx, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
        ESP_LOGI(TAG, "Got IP: %s", ip4addr_ntoa(&event->ip_info.ip));
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGE(TAG, "Disconnected from router. Retrying connection...");
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
    }
    return ESP_OK;
}

void app_main(void) {
    nvs_flash_init();
    esp_netif_init();
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_LR));

    wifi_config_t sta_config = {
        .sta = {
            .ssid = WIFI_SSID_ROUTER,
            .password = WIFI_PASS_ROUTER,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &sta_config));
    wifi_config_t ap_config = {
        .ap = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .ssid_len = strlen(WIFI_SSID),
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK,
        },
    };
    if (strlen(WIFI_PASS) == 0) {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Wait for Wi-Fi STA connection with a timeout
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, pdMS_TO_TICKS(10000));

    if (bits & CONNECTED_BIT) {
        ESP_LOGI(TAG, "Wi-Fi STA connected successfully.");
    } else {
        ESP_LOGW(TAG, "Wi-Fi STA connection failed. Falling back to AP mode only.");
        // Optionally, disable STA mode to save resources
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    }

    ESP_LOGI(TAG, "WiFi setup complete");
    xTaskCreate(tcp_task, "TCP_SERVER", configMINIMAL_STACK_SIZE + 2048, NULL, 5, NULL);

}