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
// #define TCP_SERVER_IP "172.20.10.3" 
#define TCP_PORT 8888

static const char *TAG = "wifi_station";
static const char *tcp_payload = "Hello from TCP client!";
static EventGroupHandle_t wifi_event_group;
const int CONNECTED_BIT = BIT0;

//Event Handler for WIFI Events
static esp_err_t event_handler(void *ctx, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WIFI station started. Attempting to connect to router...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
        ESP_LOGI(TAG, "Successfully connected to router. Got ip:%s", ip4addr_ntoa(&event->ip_info.ip));
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGE(TAG, "Disconnected from router. Retrying connection...");
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
    }
    return ESP_OK;
}

static void tcp_listen_task(void *pvParameters) {
    struct sockaddr_in destAddr;
    destAddr.sin_addr.s_addr = inet_addr(TCP_SERVER_IP);
    destAddr.sin_family = AF_INET;
    destAddr.sin_port = htons(TCP_PORT);

    //Create a socket
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    //Connect to the TCP client
    int err = connect(sock, (struct sockaddr *)&destAddr, sizeof(destAddr));
    if (err != 0) {
        // ESP_LOGE(TAG, "Socket unable to connect: errno %d", errno);
        ESP_LOGE(TAG, "Failed to connect to TCP server at %s:%d. Error: %d", TCP_SERVER_IP, TCP_PORT, errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Successfully connected");

    //send periodic messages
    while (1) {
        int err = send(sock, tcp_payload, strlen(tcp_payload), 0);
        if (err < 0) {
            ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
            close(sock);
            vTaskDelete(NULL);
            return;
        }
        ESP_LOGI(TAG, "Message sent");
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }
}

void app_main(void)
{
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
    // ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(78));

    wifi_config_t sta_config = {
        .sta = {
            .ssid = WIFI_SSID_ROUTER,
            .password = WIFI_PASS_ROUTER,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, portMAX_DELAY);

    wifi_config_t ap_config = {
        .ap = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .ssid_len = strlen(WIFI_SSID),
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK,
            // .ssid_hidden = false,
            // .beacon_interval = 100,
            // .authmode = WIFI_AUTH_OPEN,
        },
    };
    if (strlen(WIFI_PASS) == 0) {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &ap_config));

    gpio_reset_pin(LED_PIN);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_PIN, 1);

    ESP_LOGI(TAG, "WiFi setup complete");

    ESP_LOGI(TAG, "Starting TCP client task");
    xTaskCreate(tcp_listen_task, "tcp_client", 4096, NULL, 5, NULL);
}
