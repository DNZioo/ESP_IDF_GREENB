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

#define WIFI_SSID "ESP32"
#define WIFI_PASS "11112222"
#define PORT 8888

static const char *TAG = "wifi_station";
static char incomingPacket[255];  // buffer for incoming packets
static int sock;

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
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT,IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);
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

void tcp_server_task(void *pvParameters) {
    struct sockaddr_in destAddr;
    destAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    destAddr.sin_family = AF_INET;
    destAddr.sin_port = htons(PORT);

    //create a socket
    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    // domain: specifies the address (AF_INET for IPv4)
    //type: Specifies the socket type (SOCK_STEAM for TCP)
    //protocol : specifies the protocol (usually 0 for default protocol)
    if (sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }
    //Bind the socket
    int err = bind(sock, (struct sockaddr *)&destAddr, sizeof(destAddr));
    if (err != 0) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Socket bound successfully to port %d.", PORT);

    //Listen for incoming connections
    err = listen(sock, 1);
    if (err != 0) {
        ESP_LOGE(TAG, "Error occurred during listen: errno %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Socket listening");

    while (1) {
        //Accept a connection:
        struct sockaddr_in sourceAddr;
        uint addrLen = sizeof(sourceAddr);
        int clientSock = accept(sock, (struct sockaddr *)&sourceAddr, &addrLen);
        if (clientSock < 0) {
            ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
            close(sock);
            // vTaskDelete(NULL);
            // return;
            break;
        }

        // ESP_LOGI(TAG, "Socket accepted");
        char clientIP[16];
        inet_ntoa_r(sourceAddr.sin_addr, clientIP, sizeof(clientIP));
        ESP_LOGI(TAG, "Client connected from IP: %s, port: %d", clientIP, ntohs(sourceAddr.sin_port));

        //Receive data from the client
        while (1) {
            int len = recv(clientSock, incomingPacket, sizeof(incomingPacket) - 1, 0);
            if (len < 0) {
                ESP_LOGE(TAG, "recv failed: errno %d", errno);
                close(clientSock);
                break;
            } else if (len == 0) {
                ESP_LOGI(TAG, "Connection closed");
                close(clientSock);
                break;
            } else {
                incomingPacket[len] = 0;
                ESP_LOGI(TAG, "Received %d bytes: %s", len, incomingPacket);
            }
        }
    }
}

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
    xTaskCreate(tcp_server_task, "tcp_server", 4096, NULL, 5, NULL);
}
