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
#define SLAVE_ID 1
#define BUTTON_PIN GPIO_NUM_2

static const char *TAG = "SLAVE";

typedef struct {
    uint8_t slave_id;
    uint8_t state;     
    uint16_t checksum;
} custom_frame_t;

// // Calculate checksum
// uint16_t calculate_checksum(const uint8_t *data, size_t len) {
//     uint16_t checksum ,sum = 0, i;
//     for (i = 0; i < len; i++) 
//     sum += data[i];
//     checksum =~ sum; //1's complement
//     return checksum;
// }

// Calculate 16-bit checksum (1's complement of sum)
uint16_t calculate_checksum(const uint8_t *data, size_t len) {
    uint32_t sum = 0; // Use 32-bit to prevent overflow
    
    for (size_t i = 0; i < len; i++) {
        sum += data[i];
    }
    
    // Fold 32-bit sum to 16-bit
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    
    return (uint16_t)(~sum); // Return 16-bit 1's complement
}

void send_button_state(int sock, bool pressed) {
    uint8_t buffer[4]; // ID, State, Checksum(L), Checksum(H)
    
    buffer[0] = SLAVE_ID;
    buffer[1] = pressed ? 1 : 0;
    
    // Calculate 16-bit checksum of first 2 bytes
    uint16_t checksum = calculate_checksum(buffer, 2);
    
    // Store checksum in little-endian format
    buffer[2] = checksum & 0xFF;       // Low byte (0xFD)
    buffer[3] = (checksum >> 8) & 0xFF; // High byte (0xFF)
    
    // Send the 4-byte frame
    int sent = send(sock, buffer, 4, 0);
    if (sent < 0) {
        ESP_LOGE(TAG, "Error sending data");
    } else {
        ESP_LOGI(TAG, "Sent: ID=0x%02X, State=0x%02X, Checksum=0x%04X", 
               buffer[0], buffer[1], checksum);
    }
}

// // Send button state to the server
// void send_button_state(int sock, bool pressed) {
//     custom_frame_t frame = {
//         .slave_id = SLAVE_ID,
//         .state = pressed ? 1 : 0,
//         .checksum = 0  
//     };

//     // Calculate checksum
//     uint8_t temp_buffer[2] = {frame.slave_id, frame.state};
//     frame.checksum = calculate_checksum(temp_buffer, 2);

//     // Serialize the frame
//     uint8_t buffer[5];
//     serialize_frame(&frame, buffer);

//     // Send the serialized frame
//     int sent = send(sock, buffer, sizeof(buffer), 0);
//     if (sent < 0) {
//         ESP_LOGE(TAG, "Error sending data: errno %d", errno);
//         close(sock);
//         return;
//     } else {
//         ESP_LOGI(TAG, "Sent button state: %d", pressed);
//     }
// }

// TCP Task
static void tcp_task(void *pvParameters) {
    struct sockaddr_in destAddr;
    destAddr.sin_addr.s_addr = inet_addr(TCP_SERVER_IP);
    destAddr.sin_family = AF_INET;
    destAddr.sin_port = htons(TCP_PORT);

    int sock = -1; 
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

        // Read button state
        bool button_pressed = gpio_get_level(BUTTON_PIN) == 0; // Assuming active-low button
        send_button_state(sock, button_pressed);

        vTaskDelay(5000 / portTICK_PERIOD_MS); // Send button state every second
    }
}

// WiFi Event Handler
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WIFI station started. Attempting to connect to AP....");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGE(TAG, "WIFI disconnected. Retrying connection....");
        esp_wifi_connect();
        ESP_LOGW(TAG, "Retry to connect to the AP");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Successfully connected to WIFI. Got IP: %s", ip4addr_ntoa(&event->ip_info.ip));
    }
}

// Initialize WiFi in Station Mode
void wifi_init_sta() {
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);

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

// Main Application Entry Point
void app_main(void) {
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Configure GPIO for button
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BUTTON_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);

    // Initialize WiFi
    wifi_init_sta();

    // Start TCP Task
    ESP_LOGI(TAG, "Starting TCP task");
    xTaskCreate(tcp_task, "tcp_task", 4096, NULL, 5, NULL);
}