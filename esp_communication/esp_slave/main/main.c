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
#include "driver/uart.h"
#include "esp_log.h"


#define WIFI_SSID "ESP32"
#define WIFI_PASS "11112222"
#define DISCOVERY_PORT 8888
#define TCP_PORT 5555
#define RESPONSE_DELAY_MS 5000  
#define NET_HOUSE_ID 0x01


#define UART_NUM UART_NUM_1
#define UART_TX_PIN 17
#define UART_RX_PIN 16
#define UART_BAUD_RATE 115200


static char current_ip  [16] = {0};
static char previous_ip [16] = {0};
static char last_Pump_IP[16] = {0};


static const char *TAG      = "ESP_NETHouse";
static const char *TAG_WIFI = "WIFI";
static const char *TAG_UART = "UART_TASK";
static const char *TAG_TCP  = "TCP_TASK";
static const char *TAG_UDP  = "UDP_TASK";
static const char *TAG_NVS  = "NVS";
static const char *TAG_CRC  = "CRC";

static EventGroupHandle_t wifi_event_group;
TaskHandle_t tcp_client_handle = NULL;
const int CONNECTED_BIT = BIT0;  

typedef struct {
    uint8_t id;       // Device ID (e.g., 01 for nethouse)
    uint8_t command;  // Command byte (e.g., 0x00, 0x01, etc.)
    uint8_t crc;      // CRC-8 checksum (computed over id + command)
} frame_t;

// ============================ NVS Utility ============================

void load_previous_ip_from_nvs(char *buffer, size_t len) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("storage", NVS_READONLY, &handle);
    if (err == ESP_OK) {
        err = nvs_get_str(handle, "last_ip", buffer, &len);
        if (err == ESP_OK) {
            ESP_LOGI(TAG_NVS, "Loaded previous IP from NVS: %s", buffer);
        } else {
            ESP_LOGW(TAG_NVS, "No previous IP stored in NVS.");
        }
        nvs_close(handle);
    } else {
        ESP_LOGE(TAG_NVS, "Failed to open NVS: %s", esp_err_to_name(err));
    }
}

void save_current_ip_to_nvs(const char *ip) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        nvs_set_str(handle, "last_ip", ip);
        nvs_commit(handle);
        nvs_close(handle);
        ESP_LOGI(TAG_NVS, "Saved current IP to NVS: %s", ip);
    } else {
        ESP_LOGE(TAG_NVS, "Failed to write to NVS: %s", esp_err_to_name(err));
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
        ESP_LOGI(TAG_NVS, "Saved pump IP to NVS: %s", ip);
    }
}

void load_pump_ip_from_nvs(char *buffer, size_t len) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("storage", NVS_READONLY, &handle);
    if (err == ESP_OK) {
        err = nvs_get_str(handle, "last_pump_ip", buffer, &len);
        if (err == ESP_OK) {
            ESP_LOGI(TAG_NVS, "Loaded pump IP from NVS: %s", buffer);
        }
        nvs_close(handle);
    }
}

// ============================ Wi-Fi Events ============================

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGE(TAG_WIFI, "Wi-Fi disconnected, trying to reconnect...");
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
        ESP_LOGI(TAG, "Got ip:%s", ip4addr_ntoa(&event->ip_info.ip));
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
    xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, pdTRUE, pdFALSE, portMAX_DELAY);
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG_UDP, "Unable to create UDP socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }
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

    while (1) {

        struct sockaddr_in pump_addr;
        socklen_t addr_len = sizeof(pump_addr);
        char buffer[64];

        int len = recvfrom(sock, buffer, sizeof(buffer) - 1, 0, (struct sockaddr *)&pump_addr, &addr_len);
        if (len > 0) {
            buffer[len] = '\0';
            const char *pump_ip = inet_ntoa(pump_addr.sin_addr);
            ESP_LOGI(TAG_UDP, "Received: %s", pump_ip);
            strcpy(last_Pump_IP, pump_ip);
            save_pump_ip_to_nvs(last_Pump_IP);
        }

        // Check if the received data is valid
        if ( len > 0 ) {
            struct sockaddr_in reply_addr = {
                .sin_family = AF_INET,
                .sin_port = htons(DISCOVERY_PORT),
                .sin_addr.s_addr = inet_addr(last_Pump_IP),
            };

            // Send the ID to the pump
            uint8_t net_id = NET_HOUSE_ID;
            int sent = sendto(sock, &net_id, sizeof(net_id), 0, (struct sockaddr *)&reply_addr, sizeof(reply_addr)); 
            if (sock < 0) {
                ESP_LOGE(TAG_UDP, "Unable to create UDP socket: errno %d", errno);
                vTaskDelete(NULL);
                return;
            }            
            if (sent > 0) {
                ESP_LOGI(TAG_UDP, "Sent ID: 0x%02X", net_id);
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
    ESP_LOGI(TAG_CRC, "Verify CRC: 0x%02X", remainder);
    return remainder == 0;
}
// ============================ TCP Task Function ============================

static void tcp_client_task(void *pvParameters) {
    xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, pdTRUE, pdFALSE, portMAX_DELAY);
    const uint8_t net_id = NET_HOUSE_ID;
    int sock = -1;
        
    while (1) {
        // clear previous connection if any
        if (sock >= 0){
            close(sock);
            sock = -1;
        }

        // Load the last pump IP from NVS
        load_pump_ip_from_nvs(last_Pump_IP, sizeof(last_Pump_IP));
        if (strlen(last_Pump_IP) == 0) {
            ESP_LOGE(TAG_TCP, "No pump IP found in NVS. Exiting TCP task.");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        //Connect to pump
        struct sockaddr_in destAddr = {
            .sin_addr.s_addr = inet_addr(last_Pump_IP),
            .sin_family = AF_INET,
            .sin_port = htons(TCP_PORT)
        };

        sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (sock < 0) {
            ESP_LOGE(TAG_TCP, "Unable to create socket: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        } 
        // Connect with timeout
        int err = connect(sock, (struct sockaddr *)&destAddr, sizeof(destAddr));
        if (err != 0) {
            ESP_LOGE(TAG_TCP, "Socket connect failed: errno %d", errno);
            close(sock);
            sock = -1;
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        // Send ID
        if (send(sock, &net_id, sizeof(net_id), 0) <= 0) {
            ESP_LOGE(TAG_TCP, "ID send failed");
            close(sock);
            sock = -1;
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        ESP_LOGI(TAG_TCP, "Connected to pump: %s", last_Pump_IP);

        // send(sock, &net_id, sizeof(net_id), 0); 
        // ESP_LOGI(TAG_TCP, "Sent ID: 0x%02X", net_id);
            
        while (1) {

            // Check for disconnection
            uint8_t dummy;
            if (recv(sock, &dummy, 1, MSG_PEEK | MSG_DONTWAIT) <= 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    ESP_LOGE(TAG_TCP, "Connection lost: errno %d", errno);
                    break; // Exit inner loop to reconnect
                }
            }

            uint32_t cmd_value = 0;
            if (xTaskNotifyWait(0x00, 0xFFFFFFFF, &cmd_value, portMAX_DELAY) == pdTRUE) {
                frame_t frame = {
                    .id = NET_HOUSE_ID,
                    .command = (uint8_t)cmd_value,  
                    .crc = crc8_compute((uint8_t *)&frame, 2)
                    // .crc = crc8_compute((uint8_t *)&frame, sizeof(frame) - 1)
                };
        
                int err = send(sock, &frame, sizeof(frame), 0);
                if (err < 0) {
                    ESP_LOGE(TAG_TCP, "Error sending data: errno %d", errno);
                    break;
                }
                ESP_LOGI(TAG_TCP, "Sent: 0x%02X 0x%02X 0x%02X", frame.id, frame.command, frame.crc);
            }
            vTaskDelay(pdMS_TO_TICKS(50));


            // // check for uart command
            // if (ulTaskNotifyTake(pdTRUE, portMAX_DELAY)) {
            //     frame_t frame = {
            //         .id = NET_HOUSE_ID,
            //         .command = 0x01,  
            //         .crc = crc8_compute((uint8_t *)&frame, sizeof(frame) - 1)
            //     };

            //     int err = send(sock, &frame, sizeof(frame), 0);
            //     if (err < 0) {
            //         ESP_LOGE(TAG_TCP, "Error sending data: errno %d", errno);
            //         break;
            //     }
            //     ESP_LOGI(TAG_TCP, "Sent: 0x%02X 0x%02X 0x%02X", frame.id, frame.command, frame.crc);
            // } 

            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
}

//============================== UART TASK ===============================
void uart_task(void *pvParameters)
{
    uint8_t data[16];
    while (1) {
        int len = uart_read_bytes(UART_NUM, data, sizeof(data), pdMS_TO_TICKS(1000));
        if (len > 0) {
            // ESP_LOGI(TAG_UART, "Received: 0x%02X", data[0]);
            if(data[0] == 0x01 || data[0] == 0x00){
                //Send the cmd valve (0x01 or 0x00) to the TCP task
                xTaskNotify(tcp_client_handle, data[0], eSetValueWithoutOverwrite);  // Notify TCP task to send data
                ESP_LOGI(TAG_UART, "Notify tcp task with cmd: 0x%02X", data[0]);
            }
            // if (data[0] == 0x01) {
            //     xTaskNotifyGive(tcp_client_handle);  // Notify TCP task to send data
            //     ESP_LOGI(TAG_UART, "Notify tcp task");
            // }
            // Echo back
            uart_write_bytes(UART_NUM, (const char *)data, len);
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        vTaskDelay(pdMS_TO_TICKS(1500));
    }
}

void app_main() {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret); 

    // Initialize UART
    uart_config_t uart_config = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };
    uart_driver_install(UART_NUM, 1024, 0, 0, NULL, 0);
    uart_param_config(UART_NUM, &uart_config);
    uart_set_pin(UART_NUM, UART_TX_PIN, UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);


    wifi_event_group = xEventGroupCreate();
    wifi_init_sta();

    // Create tasks
    xTaskCreate(udp_responder_task, "udp_responder", 4096, NULL, 5, NULL);  // Increased stack size
    xTaskCreate(tcp_client_task, "tcp_server", 4096, NULL, 5, &tcp_client_handle);  // Increased stack size
    xTaskCreate(uart_task, "uart_task", 4096, NULL, 5, NULL);  // Increased stack size
}