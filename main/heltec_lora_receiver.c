#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_idf_version.h"
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
#include "rom/ets_sys.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "HELTEC_LORA_RX";

// Heltec WiFi LoRa 32 V3 (ESP32-S3 + SX1262) default pin map.
// Adjust if your board variant differs.
#define PIN_NUM_MOSI 10
#define PIN_NUM_MISO 11
#define PIN_NUM_SCLK 9
#define PIN_NUM_NSS 8
#define PIN_NUM_RST 12
#define PIN_NUM_BUSY 13
#define PIN_NUM_DIO1 14

// LoRa radio settings (must match transmitter)
#define RF_FREQUENCY_HZ 915000000U
#define LORA_SF 0x07      // SF7
#define LORA_BW 0x04      // 125 kHz
#define LORA_CR 0x01      // 4/5
#define LORA_LDRO 0x00    // low data rate optimize off
#define LORA_PREAMBLE 8U

#define RX_BUFFER_MAX 255

// SX126x opcodes
#define OPCODE_SET_STANDBY 0x80
#define OPCODE_SET_PACKET_TYPE 0x8A
#define OPCODE_SET_RF_FREQUENCY 0x86
#define OPCODE_SET_BUFFER_BASE_ADDRESS 0x8F
#define OPCODE_SET_MODULATION_PARAMS 0x8B
#define OPCODE_SET_PACKET_PARAMS 0x8C
#define OPCODE_SET_DIO_IRQ_PARAMS 0x08
#define OPCODE_SET_DIO2_AS_RF_SWITCH_CTRL 0x9D
#define OPCODE_SET_RX 0x82
#define OPCODE_GET_IRQ_STATUS 0x12
#define OPCODE_CLEAR_IRQ_STATUS 0x02
#define OPCODE_GET_RX_BUFFER_STATUS 0x13
#define OPCODE_READ_BUFFER 0x1E

#define PACKET_TYPE_LORA 0x01
#define STDBY_RC 0x00

#define IRQ_RX_DONE (1U << 1)
#define IRQ_HEADER_ERR (1U << 5)
#define IRQ_CRC_ERR (1U << 6)

typedef struct {
    char gps_location[96];
    char timestamp[64];
    int bt_count;
    int wf_count;
} detection_packet_t;

static spi_device_handle_t radio_spi;

static inline void lora_delay_us(uint32_t us) {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    esp_rom_delay_us(us);
#else
    ets_delay_us(us);
#endif
}

static void wait_while_busy(void) {
    while (gpio_get_level(PIN_NUM_BUSY)) {
        lora_delay_us(100);
    }
}

static esp_err_t radio_write_cmd(uint8_t opcode, const uint8_t *data, size_t len) {
    wait_while_busy();

    uint8_t tx[260] = {0};
    if (len > (sizeof(tx) - 1)) {
        return ESP_ERR_INVALID_SIZE;
    }

    tx[0] = opcode;
    if (len > 0 && data != NULL) {
        memcpy(&tx[1], data, len);
    }

    spi_transaction_t t = {
        .length = (len + 1) * 8,
        .tx_buffer = tx,
    };

    esp_err_t err = spi_device_transmit(radio_spi, &t);
    wait_while_busy();
    return err;
}

static esp_err_t radio_read_cmd(uint8_t opcode, uint8_t *out, size_t out_len) {
    wait_while_busy();

    uint8_t tx[260] = {0};
    uint8_t rx[260] = {0};

    if (out_len > (sizeof(tx) - 2)) {
        return ESP_ERR_INVALID_SIZE;
    }

    tx[0] = opcode;
    tx[1] = 0x00; // NOP

    spi_transaction_t t = {
        .length = (out_len + 2) * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };

    esp_err_t err = spi_device_transmit(radio_spi, &t);
    if (err == ESP_OK && out_len > 0) {
        memcpy(out, &rx[2], out_len);
    }

    wait_while_busy();
    return err;
}

static void radio_hard_reset(void) {
    gpio_set_level(PIN_NUM_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_NUM_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
}

static esp_err_t radio_init(void) {
    esp_err_t err;

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIN_NUM_RST) | (1ULL << PIN_NUM_NSS),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    gpio_config_t in_conf = {
        .pin_bit_mask = (1ULL << PIN_NUM_BUSY) | (1ULL << PIN_NUM_DIO1),
        .mode = GPIO_MODE_INPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&in_conf));

    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 8 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = PIN_NUM_NSS,
        .queue_size = 1,
    };

    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &devcfg, &radio_spi));

    radio_hard_reset();

    uint8_t standby[] = {STDBY_RC};
    ESP_ERROR_CHECK(radio_write_cmd(OPCODE_SET_STANDBY, standby, sizeof(standby)));

    uint8_t packet_type[] = {PACKET_TYPE_LORA};
    ESP_ERROR_CHECK(radio_write_cmd(OPCODE_SET_PACKET_TYPE, packet_type, sizeof(packet_type)));

    uint32_t frf = (uint32_t)((double)RF_FREQUENCY_HZ / (32000000.0 / 33554432.0));
    uint8_t freq[4] = {(uint8_t)(frf >> 24), (uint8_t)(frf >> 16), (uint8_t)(frf >> 8), (uint8_t)frf};
    ESP_ERROR_CHECK(radio_write_cmd(OPCODE_SET_RF_FREQUENCY, freq, sizeof(freq)));

    uint8_t rf_switch[] = {0x01};
    ESP_ERROR_CHECK(radio_write_cmd(OPCODE_SET_DIO2_AS_RF_SWITCH_CTRL, rf_switch, sizeof(rf_switch)));

    uint8_t buf_base[] = {0x00, 0x00};
    ESP_ERROR_CHECK(radio_write_cmd(OPCODE_SET_BUFFER_BASE_ADDRESS, buf_base, sizeof(buf_base)));

    uint8_t mod_params[] = {LORA_SF, LORA_BW, LORA_CR, LORA_LDRO};
    ESP_ERROR_CHECK(radio_write_cmd(OPCODE_SET_MODULATION_PARAMS, mod_params, sizeof(mod_params)));

    uint8_t pkt_params[] = {
        (uint8_t)(LORA_PREAMBLE >> 8),
        (uint8_t)(LORA_PREAMBLE & 0xFF),
        0x00, // explicit header
        RX_BUFFER_MAX,
        0x01, // CRC on
        0x00  // standard IQ
    };
    ESP_ERROR_CHECK(radio_write_cmd(OPCODE_SET_PACKET_PARAMS, pkt_params, sizeof(pkt_params)));

    uint8_t irq_cfg[] = {
        0x00,
        0x62, // enable RxDone + HeaderErr + CrcErr
        0x00,
        0x62, // map those to DIO1
        0x00,
        0x00,
        0x00,
        0x00,
    };
    err = radio_write_cmd(OPCODE_SET_DIO_IRQ_PARAMS, irq_cfg, sizeof(irq_cfg));
    return err;
}

static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    if (*s == '\0') return s;

    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }
    return s;
}

static bool parse_detection_packet(char *payload, detection_packet_t *out) {
    char *s = trim(payload);

    if (*s == '(') s++;
    size_t slen = strlen(s);
    if (slen > 0) {
        char *last = s + slen - 1;
        if (*last == ')') *last = '\0';
    }

    char *parts[4] = {0};
    int idx = 0;
    char *token = strtok(s, ",");
    while (token && idx < 4) {
        parts[idx++] = trim(token);
        token = strtok(NULL, ",");
    }
    if (idx != 4 || strtok(NULL, ",") != NULL) {
        return false;
    }

    snprintf(out->gps_location, sizeof(out->gps_location), "%s", parts[0]);
    snprintf(out->timestamp, sizeof(out->timestamp), "%s", parts[1]);
    out->bt_count = atoi(parts[2]);
    out->wf_count = atoi(parts[3]);
    return true;
}

static esp_err_t radio_start_rx_continuous(void) {
    uint8_t timeout[3] = {0xFF, 0xFF, 0xFF}; // continuous RX
    return radio_write_cmd(OPCODE_SET_RX, timeout, sizeof(timeout));
}

static esp_err_t radio_get_irq(uint16_t *irq) {
    uint8_t b[2] = {0};
    esp_err_t err = radio_read_cmd(OPCODE_GET_IRQ_STATUS, b, sizeof(b));
    if (err != ESP_OK) return err;
    *irq = (uint16_t)((b[0] << 8) | b[1]);
    return ESP_OK;
}

static esp_err_t radio_clear_irq(uint16_t irq) {
    uint8_t b[2] = {(uint8_t)(irq >> 8), (uint8_t)(irq & 0xFF)};
    return radio_write_cmd(OPCODE_CLEAR_IRQ_STATUS, b, sizeof(b));
}

static esp_err_t radio_read_rx_payload(uint8_t *buf, uint8_t *size) {
    uint8_t status[2] = {0};
    ESP_ERROR_CHECK(radio_read_cmd(OPCODE_GET_RX_BUFFER_STATUS, status, sizeof(status)));

    const uint16_t rx_payload_len = status[0];
    const uint8_t start_ptr = status[1];

    if (rx_payload_len == 0U || rx_payload_len > (uint16_t)RX_BUFFER_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t tx[260] = {OPCODE_READ_BUFFER, start_ptr, 0x00};
    uint8_t rx[260] = {0};

    spi_transaction_t t = {
        .length = (rx_payload_len + 3U) * 8U,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };

    wait_while_busy();
    ESP_ERROR_CHECK(spi_device_transmit(radio_spi, &t));
    wait_while_busy();

    memcpy(buf, &rx[3], rx_payload_len);
    *size = (uint8_t)rx_payload_len;
    return ESP_OK;
}

void app_main(void) {
    ESP_LOGI(TAG, "Starting ESP-IDF Heltec LoRa receiver...");
    ESP_ERROR_CHECK(radio_init());
    ESP_ERROR_CHECK(radio_start_rx_continuous());

    uint8_t payload[RX_BUFFER_MAX + 1] = {0};

    while (1) {
        if (gpio_get_level(PIN_NUM_DIO1) == 1) {
            uint16_t irq = 0;
            if (radio_get_irq(&irq) == ESP_OK) {
                if (irq & IRQ_RX_DONE) {
                    uint8_t size = 0;
                    if (radio_read_rx_payload(payload, &size) == ESP_OK) {
                        payload[size] = '\0';
                        ESP_LOGI(TAG, "Raw packet: %s", (char *)payload);

                        detection_packet_t parsed = {0};
                        char parse_buf[RX_BUFFER_MAX + 1];
                        snprintf(parse_buf, sizeof(parse_buf), "%s", (char *)payload);

                        if (parse_detection_packet(parse_buf, &parsed)) {
                            ESP_LOGI(TAG, "--- LoRa Packet Received ---");
                            ESP_LOGI(TAG, "GPS Location: %s", parsed.gps_location);
                            ESP_LOGI(TAG, "Time: %s", parsed.timestamp);
                            ESP_LOGI(TAG, "BT Detection Count: %d", parsed.bt_count);
                            ESP_LOGI(TAG, "WF Detection Count: %d", parsed.wf_count);
                        } else {
                            ESP_LOGW(TAG, "Invalid format. Expected (GPS_LOCATION, TIME, BT_DETECTION_COUNT, WF_DETECTION_COUNT)");
                        }
                    }
                }

                if (irq & (IRQ_HEADER_ERR | IRQ_CRC_ERR)) {
                    ESP_LOGW(TAG, "LoRa header/CRC error (IRQ=0x%04X)", irq);
                }

                radio_clear_irq(irq);
                radio_start_rx_continuous();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
