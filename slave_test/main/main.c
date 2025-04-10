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

#define WIFI_SSID "DNZio"
#define WIFI_PASS "11112222"
#define TCP_SERVER_IP "192.168.4.1"
#define TCP_PORT 8888
#define SLAVE_ID 1
#define BUTTON_PIN GPIO_NUM_2

static const char *TAG = "SLAVE";
bool connected = false;
int sock = -1; 

// Custom Frame Definitions
typedef enum {
    FRAME_TYPE_SENSOR = 0x01,  // Sensor data frame
    FRAME_TYPE_COMMAND = 0x02   // Command/button frame
} frame_type_t;

typedef struct {
    uint16_t moisture;  
    uint16_t ec;        
    int16_t  temp;      
} sensor_data_t;

typedef struct {
    uint8_t slave_id;   // 0x01
    frame_type_t type;  // FRAME_TYPE_SENSOR/COMMAND
    union {
        sensor_data_t sensors;
        uint8_t command;
    } data;
    uint16_t checksum;
} custom_frame_t;

uint16_t calculate_checksum(const custom_frame_t *frame) {
    uint32_t sum = 0;
    
    // Common fields for all frame types
    sum += frame->slave_id;
    sum += frame->type;
    
    // Handle different frame types
    if (frame->type == FRAME_TYPE_SENSOR) {
        // Sensor data frame
        sum += frame->data.sensors.moisture;
        sum += frame->data.sensors.ec;
        sum += (uint16_t)frame->data.sensors.temp; // Handle signed value
    } else {
        // Command frame
        sum += frame->data.command;
    }
    
    // Fold 32-bit sum to 16-bit
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    
    return (uint16_t)(~sum);  // One's complement
}

// --- Send Sensor Data (10s interval) ---
void send_sensor_data() {
    custom_frame_t frame = {
        .slave_id = 0x01,
        .type = FRAME_TYPE_SENSOR,
        .data.sensors = {50, 1200, 25},
        .checksum = 0  // Will be calculated
    };
    
    frame.checksum = calculate_checksum(&frame);

    int sent = send(sock, &frame, sizeof(frame), 0);
    if (sent < 0) {
        ESP_LOGE(TAG, "Error sending sensor data");
        close(sock);
        connected = false;
    } else {
        ESP_LOGI(TAG, "Sent data: ID = %d, type = %d, Moisture=%d, EC=%d, Temp=%d, checksum = 0x%04X", 
                frame.slave_id, frame.type, frame.data.sensors.moisture, frame.data.sensors.ec, frame.data.sensors.temp, frame.checksum);
    }
}

void send_button_state() {
    bool button_pressed = gpio_get_level(BUTTON_PIN) == 1;

    custom_frame_t frame = {
        .slave_id = SLAVE_ID,
        .type = FRAME_TYPE_COMMAND,
        .data.command = button_pressed ? 1 : 0,
        .checksum = 0  // Initialize before calculation
    };

    frame.checksum = calculate_checksum(&frame);

    int sent = send(sock, &frame, sizeof(frame), 0);
    if (sent < 0) {
        ESP_LOGE(TAG, "Error sending command");
        close(sock);
        connected = false;
    } else {
        ESP_LOGI(TAG, "Sent cmd: ID = %d, type = %d, button = %d, Checksum=0x%04X", 
                frame.slave_id, frame.type, frame.data.command, frame.checksum);
    }
}

// --- Task for Sensor Data (10s) ---
void sensor_task(void *pvParameters) {
    while (1) {
        if (connected) {
            send_sensor_data();
        }
        vTaskDelay(10000 / portTICK_PERIOD_MS); // 10s delay
    }
}

// --- Task for Button Commands (5s) ---
void command_task(void *pvParameters) {
    while (1) {
        if (connected) {
            send_button_state();
        }
        vTaskDelay(5000 / portTICK_PERIOD_MS); // 5s delay
    }
}

static void tcp_task(void *pvParameters) {
    struct sockaddr_in destAddr = {
        .sin_addr.s_addr = inet_addr(TCP_SERVER_IP),
        .sin_family = AF_INET,
        .sin_port = htons(TCP_PORT)
    };

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
        vTaskDelay(5000 / portTICK_PERIOD_MS);
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
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
    wifi_init_sta();

    xTaskCreate(tcp_task, "tcp_task", 4096, NULL, 5, NULL);
    xTaskCreate(sensor_task, "sensor_task", 2048, NULL, 5, NULL);
    xTaskCreate(command_task, "command_task", 2048, NULL, 5, NULL);

}