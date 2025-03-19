#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "driver/gpio.h"

#define UART_PORT_NUM      UART_NUM_2
#define UART_TX_PIN        GPIO_NUM_17  
#define UART_RX_PIN        GPIO_NUM_16  
#define BUF_SIZE           1024
static const char *TAG = "UART_COMMS";

void app_main(void)
{
    // Configure UART parameters
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };

    // Install UART driver
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT_NUM, BUF_SIZE * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT_NUM, UART_TX_PIN, UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    ESP_LOGI(TAG, "UART initialized successfully");
    uint8_t rx_buffer[BUF_SIZE];            // Buffer to store received data
    uint8_t tx_buffer[BUF_SIZE];            // Buffer to store transmitted data

    while (1) {
        // Read data from UART
        int len = uart_read_bytes(UART_PORT_NUM, rx_buffer, BUF_SIZE - 1, 20 / portTICK_PERIOD_MS);
        if (len > 0) {
            rx_buffer[len] = '\0';          // Null-terminate the received string
            ESP_LOGI(TAG, "Received: %s", rx_buffer);

            // Prepare a response to send back to STM32
            size_t max_rx_len = sizeof(tx_buffer) - strlen("ESP32 Received: ") - 1;
            snprintf((char *)tx_buffer, sizeof(tx_buffer), "ESP32 Received: %.*s", (int)max_rx_len, rx_buffer);

            // Send the response back to STM32
            uart_write_bytes(UART_PORT_NUM, (const char *)tx_buffer, strlen((char *)tx_buffer));
        }
    }
}