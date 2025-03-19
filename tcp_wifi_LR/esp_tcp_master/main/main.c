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
#define LED_PIN GPIO_NUM_5
#define TCP_SERVER_IP "192.168.4.2"
#define TCP_PORT 8888

static const char *TAG = "wifi_station";
static const char *tcp_payload = "Hello from TCP client!";

static EventGroupHandle_t wifi_event_group;
const int CONNECTED_BIT = BIT0;

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

// static void tcp_client_task(void *pvParameters) {
//     struct sockaddr_in destAddr;
//     destAddr.sin_addr.s_addr = inet_addr(TCP_SERVER_IP);
//     destAddr.sin_family = AF_INET;
//     destAddr.sin_port = htons(TCP_PORT);

//     int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
//     if (sock < 0) {
//         ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
//         vTaskDelete(NULL);
//         return;
//     }

//     int err = connect(sock, (struct sockaddr *)&destAddr, sizeof(destAddr));
//     if (err != 0) {
//         ESP_LOGE(TAG, "Socket unable to connect: errno %d", errno);
//         close(sock);
//         vTaskDelete(NULL);
//         return;
//     }

//     ESP_LOGI(TAG, "Successfully connected");

//     while (1) {
//         int err = send(sock, tcp_payload, strlen(tcp_payload), 0);
//         if (err < 0) {
//             ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
//             close(sock);
//             vTaskDelete(NULL);
//             return;
//         }
//         ESP_LOGI(TAG, "Message sent");
//         vTaskDelay(2000 / portTICK_PERIOD_MS);
//     }
// }

// static void tcp_client_task(void *pvParameters) {
//     struct sockaddr_in destAddr;
//     destAddr.sin_addr.s_addr = inet_addr(TCP_SERVER_IP);
//     destAddr.sin_family = AF_INET;
//     destAddr.sin_port = htons(TCP_PORT);

//     int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
//     if (sock < 0) {
//         ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
//         vTaskDelete(NULL);
//         return;
//     }
//     int err = connect(sock, (struct sockaddr *)&destAddr, sizeof(destAddr));
//     if (err != 0) {
//         ESP_LOGE(TAG, "Socket unable to connect: errno %d", errno);
//         close(sock);
//         vTaskDelete(NULL);
//         return;
//     }

//     ESP_LOGI(TAG, "Successfully connected to TCP server");

//     while (1) {
//         // Send a message to the slave
//         int err = send(sock, tcp_payload, strlen(tcp_payload), 0);
//         if (err < 0) {
//             ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
//             close(sock);
//             vTaskDelete(NULL);
//             return;
//         }
//         ESP_LOGI(TAG, "Message sent: %s", tcp_payload);

//         // Wait for a response from the slave
//         char buffer[256];
//         int len = recv(sock, buffer, sizeof(buffer) - 1, 0);
//         if (len < 0) {
//             ESP_LOGE(TAG, "Error occurred during receiving: errno %d", errno);
//         } else if (len == 0) {
//             ESP_LOGW(TAG, "Connection closed by the server");
//         } else {
//             buffer[len] = 0; // Null-terminate the received data
//             ESP_LOGI(TAG, "Received %d bytes: %s", len, buffer);
//         }

//         // Delay before sending the next message
//         vTaskDelay(2000 / portTICK_PERIOD_MS);
//     }
// }

static void tcp_client_task(void *pvParameters) {
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

        // Send a message to the server
        int err = send(sock, tcp_payload, strlen(tcp_payload), 0);
        if (err < 0) {
            ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
            close(sock); // Close the socket on failure
            connected = false;
            vTaskDelay(2000 / portTICK_PERIOD_MS); // Wait before retrying
            continue;
        }
        ESP_LOGI(TAG, "Message sent: %s", tcp_payload);

        // Wait for a response from the server
        char buffer[256];
        int len = recv(sock, buffer, sizeof(buffer) - 1, 0);
        if (len < 0) {
            ESP_LOGE(TAG, "Error occurred during receiving: errno %d", errno);
            close(sock); // Close the socket on failure
            connected = false;
            vTaskDelay(2000 / portTICK_PERIOD_MS); // Wait before retrying
            continue;
        } else if (len == 0) {
            ESP_LOGW(TAG, "Connection closed by the server");
            close(sock); // Close the socket if the server closes the connection
            connected = false;
            vTaskDelay(2000 / portTICK_PERIOD_MS); // Wait before retrying
            continue;
        } else {
            buffer[len] = 0; // Null-terminate the received data
            ESP_LOGI(TAG, "Received %d bytes: %s", len, buffer);
        }

        // Delay before sending the next message
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }
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
    // ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR))

    wifi_config_t sta_config = {
        .sta = {
            .ssid = WIFI_SSID_ROUTER,
            .password = WIFI_PASS_ROUTER,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &sta_config));
    // ESP_ERROR_CHECK(esp_wifi_start());
    // xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, portMAX_DELAY);
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

    gpio_reset_pin(LED_PIN);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_PIN, 1);

    ESP_LOGI(TAG, "WiFi setup complete");

    ESP_LOGI(TAG, "Starting TCP client task");
    xTaskCreate(tcp_client_task, "tcp_client", 4096, NULL, 5, NULL);
}