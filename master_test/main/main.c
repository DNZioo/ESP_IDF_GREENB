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
#include "esp_event_loop.h"
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

static const char *TAG = "TCP_MASTER";
static int sock;

static EventGroupHandle_t wifi_event_group;
const int CONNECTED_BIT = BIT0;

typedef struct {
    uint8_t address;         // Target device address (1 byte)
    uint8_t function_code;   // Function code (1 byte)
    uint16_t data_len;       // Length of the payload (2 bytes)
    uint8_t data[256];       // Payload data (variable length, up to 256 bytes)
    uint16_t checksum;       // Error-checking mechanism (2 bytes)
} modbus_frame_t;

// Calculate checksum
uint16_t calculate_checksum(const uint8_t *data, size_t len) {
    uint16_t checksum = 0;
    for (size_t i = 0; i < len; i++) {
        checksum += data[i];
    }
    return checksum;
}

// Serialize frame into raw bytes
void serialize_frame(uint8_t *buffer, const modbus_frame_t *frame) {
    buffer[0] = frame->address;
    buffer[1] = frame->function_code;
    buffer[2] = (frame->data_len >> 8) & 0xFF; 
    buffer[3] = frame->data_len & 0xFF;

    memcpy(&buffer[4], frame->data, frame->data_len);

    uint16_t checksum = calculate_checksum(buffer, 4 + frame->data_len); // Exclude checksum field
    buffer[4 + frame->data_len] = (checksum >> 8) & 0xFF; // Big-endian
    buffer[5 + frame->data_len] = checksum & 0xFF;
}
void handle_client(int client_sock) {
    modbus_frame_t frame;
    memset(&frame, 0, sizeof(frame));

    // Prepare the frame
    frame.address = 0x02;       // Target slave address
    frame.function_code = 0x03; // Example function code
    const char *message = "Hello";
    frame.data_len = strlen(message);
    memcpy(frame.data, message, frame.data_len);

    // Serialize the frame
    uint8_t buffer[264]; // Max frame size: 1 + 1 + 2 + 256 + 2
    serialize_frame(buffer, &frame);

    // Send the frame
    int sent = send(client_sock, buffer, 4 + frame.data_len + 2, 0); // 4 bytes for header, data_len, and 2 bytes for checksum
    if (sent < 0) {
        ESP_LOGE(TAG, "Error sending data: errno %d", errno);
        close(client_sock);
        return;
    }
    ESP_LOGI(TAG, "Sent frame: Address=%d, Function=%d, Data=%s", frame.address, frame.function_code, frame.data);

    // Wait for acknowledgment or keep the connection alive briefly
    vTaskDelay(1000 / portTICK_PERIOD_MS); // Keep the connection open for 2 seconds
}


//TCP task
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
    ESP_LOGI(TAG, "Socket bound successfully to port %d.", PORT);

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
            continue; //continue listening for new connections
        }

        char clientIP[16];
        inet_ntoa_r(sourceAddr.sin_addr, clientIP, sizeof(clientIP));
        ESP_LOGI(TAG, "Client connected from IP: %s, port: %d", clientIP, ntohs(sourceAddr.sin_port));

        handle_client(clientSock);
        close(clientSock);
    }
}
static esp_err_t event_handler(void *ctx, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
        ESP_LOGI(TAG, "got ip:%s", ip4addr_ntoa(&event->ip_info.ip));
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