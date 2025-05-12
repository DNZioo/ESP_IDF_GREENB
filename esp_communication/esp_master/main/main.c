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
#include "driver/uart.h"

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


//UART PINs configuration
#define TXD_PIN (GPIO_NUM_17)
#define RXD_PIN (GPIO_NUM_16)
#define UART_NUM UART_NUM_1
#define UART_BAUD_RATE 115200

static const char *TAG = "ESP_PUMP";
static const char *TAG_TCP = "TCP";
static const char *TAG_UDP = "UDP";
static const char *TAG_WIFI = "WIFI";
static const char *TAG_UART = "UART";
static const char *TAG_NVS = "NVS";

static EventGroupHandle_t wifi_event_group;
TaskHandle_t tcp_task_handle = NULL;

static char current_ip [MAX_IP_LENGTH] = {0}; 
static char previous_ip[MAX_IP_LENGTH] = {0}; 

//crc remainder
static uint8_t remainder = 0x00; // CRC remainder

// ============================ Handle Net Houses ============================

typedef struct {
    int sock;
    char id[16];
    struct sockaddr_in addr;
} net_client_t;
static net_client_t net_client[5] = {0};

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

// ============================ Wi-Fi Events ============================


static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
        ESP_LOGE(TAG_WIFI, "Retry to connect to the AP");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        strncpy(current_ip, ip4addr_ntoa(&event->ip_info.ip), MAX_IP_LENGTH - 1);
        // current_ip[sizeof(current_ip) - 1] = '\0';

        //load and save IP addresses
        load_previous_ip_from_nvs(previous_ip, sizeof(previous_ip));
        save_current_ip_to_nvs(current_ip);
        ESP_LOGI(TAG_WIFI, "IP changed: %s to %s", current_ip, previous_ip);

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
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG_UDP, "Failed to create socket: errno %d", errno);
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

    while (1) {
        xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, false, true, portMAX_DELAY);  

        //Send broadcast message
        if (strcmp(previous_ip, current_ip) != 0 || !nethouse_responded) {
            strncpy(previous_ip, current_ip, sizeof(previous_ip) - 1);
            sendto(sock, current_ip, strlen(current_ip), 0, (struct sockaddr *)&broadcast_addr, sizeof(broadcast_addr));
            ESP_LOGI(TAG_UDP, "PUMP is broadcasting: %s", current_ip);
        }
        
        struct sockaddr_in slave_addr;
        socklen_t addr_len = sizeof(slave_addr);

        uint8_t response[1] = {0};  
        int received = recvfrom(sock, response, sizeof(response), 0, (struct sockaddr *)&slave_addr, &addr_len);
        if (received > 0) {
            // Handle the response
            ESP_LOGI(TAG_UDP, "nethouse:%d", response[0]);
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
    remainder = crc8_compute(data, len_with_crc);
    // ESP_LOGI(TAG, "Verify CRC: 0x%02X", remainder);
    return remainder == 0;
}

// ============================ TCP Task ============================


void tcp_server_task(void *pvParameters) {
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, false, true, portMAX_DELAY);
    
    int pump_sock;  //Main socket for the pump
    struct sockaddr_in pump_addr, net_addr;
    socklen_t addr_len = sizeof(net_addr);

    pump_addr.sin_family = AF_INET;
    pump_addr.sin_port = htons(TCP_PORT);
    pump_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    pump_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (pump_sock < 0) {
        ESP_LOGE(TAG_TCP, "Failed to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    int err = bind(pump_sock, (struct sockaddr *)&pump_addr, sizeof(pump_addr));
    if (err != 0) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        close(pump_sock);
        vTaskDelete(NULL);
        return;
    }

    err = listen(pump_sock, 5);
    if (err != 0) {
        ESP_LOGE(TAG, "Error occurred during listen: errno %d", errno);
        close(pump_sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Socket listening");

    // Assume both clients want pump on
    uint8_t net_cmd[1] = {0x01};
    
    while (1) {
        
        fd_set read_fds;                //Create a set of file descriptors
        FD_ZERO(&read_fds);             //Initialize the set to empty
        FD_SET(pump_sock, &read_fds);   //Add the pump socket to the set
        int max_fd = pump_sock;         // Initialize max_fd to the pump socket

        // Add client sockets to the set
        for (int i = 0; i < 5; i++) {
            if (net_client[i].sock > 0) {
                FD_SET(net_client[i].sock, &read_fds);
                if (net_client[i].sock > max_fd) max_fd = net_client[i].sock;
            }
        }

        // Wait for activity on the sockets
        int activity = select(max_fd + 1, &read_fds, NULL, NULL, NULL);
        if ((activity < 0) && (errno != EINTR)) {
            ESP_LOGE(TAG, "Select error: errno %d", errno);
            continue;
        }

        //haldle new connection
        if (FD_ISSET(pump_sock, &read_fds)) {
            int new_socket = accept(pump_sock, (struct sockaddr *)&net_addr, &addr_len);
            if (new_socket < 0) {
                ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
                continue;
            }
            ESP_LOGI(TAG, "New connection accepted");

            // Receive ID from the new socket
            uint8_t id_buffer[1] = {0};
            int len_id = recv(new_socket, id_buffer, sizeof(id_buffer), 0);
            if (len_id == 1) {
                for (int i = 0; i < 5; i++) {
                    if (net_client[i].sock == 0) {
                        net_client[i].sock = new_socket;
                        snprintf(net_client[i].id, sizeof(net_client[i].id), "%02X", id_buffer[0]);
                        net_client[i].addr = net_addr;
                        ESP_LOGI(TAG_TCP, "Registered from Net house: 0x%02X", id_buffer[0]);
                        break;
                    }
                }
            } else {
                ESP_LOGW(TAG_TCP, "Invalid ID message from nethouse");
                close(new_socket);
            }
        }

        // Handle existing connections
        for (int i = 0; i < 5; i++) {
            int sd = net_client[i].sock;
            if (sd > 0 && FD_ISSET(sd, &read_fds)) {
                uint8_t buffer[64] = {0};
                int len_recv = recv(sd, buffer, sizeof(buffer) - 1, 0);
                if (len_recv <= 0) {
                    close(sd);
                    net_client[i].sock = 0;
                    memset(net_client[i].id, 0, sizeof(net_client[i].id));
                    ESP_LOGI(TAG_TCP, "Client disconnected");
                    continue;
                } else {
                    buffer[len_recv] = '\0';
                    ESP_LOGI(TAG_TCP, "NETHouse:%s socket:%d:0x%02X 0x%02X 0x%02X", net_client[i].id, net_client[i].sock, buffer[0], buffer[1], buffer[2]);
                    // Verify CRC
                    if (crc8_verify(buffer, len_recv)) {
                        ESP_LOGI(TAG_TCP, "CRC verify: 0x%02X 0x%02X 0x%02X", buffer[0], buffer[1], remainder);
                        // Process the data
                        uint8_t cmd = buffer[1];
                        if (cmd == 0x00 || cmd == 0x01){
                            // send uart command 
                            uart_write_bytes(UART_NUM, &cmd, 1);
                            net_cmd[i] = cmd;

                            if (net_cmd[0] == 0x00){
                                ESP_LOGI(TAG_TCP, "pump off");
                            } else{
                                ESP_LOGI(TAG_TCP, "pump on");
                            }
                        }
                    } else {
                        ESP_LOGW(TAG_TCP, "CRC is invalid");
                    }  
                }
            }
            // if (strcmp(net_client[i].id, "01") == 0) {
            //     send(sd, "heyA", strlen("heyA"), 0);
            //     ESP_LOGI(TAG_TCP, "Sent 'heyA' to nethouse 01");
            // } else if (strcmp(net_client[i].id, "02") == 0) {
            //     send(sd, "heyB", strlen("heyB"), 0);
            //     ESP_LOGI(TAG_TCP, "Sent 'heyB' to nethouse 02");
            // }
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

// ============================ Main Function ============================

void app_main() {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret); 
    wifi_event_group = xEventGroupCreate();
    wifi_init_sta();

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
    uart_set_pin(UART_NUM, TXD_PIN, RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    
    xTaskCreate(udp_broadcast_task, "udp_server", 4096, NULL, 5, NULL);
    xTaskCreate(tcp_server_task, "tcp_server", 4096, NULL, 5, NULL);
    // xTaskCreate(uart_task, "uart_task", 2048, NULL, 5, NULL);
}