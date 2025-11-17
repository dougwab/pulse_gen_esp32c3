#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_random.h"

#define GPIO_OUT_1          4
#define GPIO_OUT_2          5
#define LOG_TAG             "pulse_generator"
#define MAX_INTERVAL_SECONDS 3600
#define MAX_INTERVAL_MS     (MAX_INTERVAL_SECONDS * 1000)
#define MIN_INTERVAL_MS     200
#define MIN_PULSE_MS        100
#define MAX_PULSE_MS        10000
#define UART_BUFFER_SIZE    1024
#define UART_PORT           UART_NUM_0
#define UART_BAUD_RATE      115200

typedef enum {
    MODE_DEFINED,
    MODE_RANDOM
} pulse_mode_t;

typedef struct {
    int gpio;
    int interval_ms;
    int pulse_duration_ms;
    pulse_mode_t mode;
    const char *label;
} pulse_config_t;

// ---------- UART ----------
static void configure_uart(void) {
    uart_config_t uart_config = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &uart_config));
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, UART_BUFFER_SIZE, 0, 0, NULL, 0));
}

static char uart_read_char(void) {
    uint8_t c;
    while (uart_read_bytes(UART_PORT, &c, 1, portMAX_DELAY) <= 0) {}
    return (char)c;
}

// ---------- GPIO ----------
static void configure_gpio(int gpio) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << gpio),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    gpio_set_level(gpio, 1);
}

// ---------- Interface ----------
static int read_int_from_uart(const char *prompt, int min_val, int max_val) {
    char input[10] = {0};
    int index = 0;
    char c;
    
    printf("%s (%d a %d ms): ", prompt, min_val, max_val);
    
    while (index < 9) {
        c = uart_read_char();
        if (c == '\r' || c == '\n') {
            if (index > 0) break;
        } else if (c >= '0' && c <= '9') {
            putchar(c);
            input[index++] = c;
        }
    }
    putchar('\n');
    
    int value = atoi(input);
    if (value < min_val || value > max_val) {
        printf("Valor fora do intervalo! Deve ser entre %d e %d ms.\n", min_val, max_val);
        return -1;
    }
    return value;
}

static int ask_number_of_outputs(void) {
    printf("=== GERADOR DE PULSOS ===\n");
    printf("Quantas saídas deseja usar?\n");
    printf("1 - Apenas Saída 1 (GPIO4)\n");
    printf("2 - Ambas saídas (GPIO4 e GPIO5)\n");
    printf("Digite 1 ou 2: ");

    char c;
    do {
        c = uart_read_char();
    } while (c != '1' && c != '2');

    printf("%c\n", c);
    return (c - '0');
}

static pulse_mode_t select_mode(int interval_ms) {
    printf("Selecione o modo:\n");
    printf("D - Definido (intervalo fixo de %d ms)\n", interval_ms);
    printf("R - Aleatório (de 1 a %d ms)\n", interval_ms);
    printf("Digite D ou R: ");

    char c;
    do {
        c = uart_read_char();
    } while (c != 'D' && c != 'd' && c != 'R' && c != 'r');

    printf("%c\n", c);
    return (c == 'D' || c == 'd') ? MODE_DEFINED : MODE_RANDOM;
}

static void wait_for_start(void) {
    printf("\nPressione 'S' para iniciar...\n");
    char c;
    do {
        c = uart_read_char();
    } while (c != 'S' && c != 's');
    printf("\n>>> Iniciando geração de pulsos...\n");
}

// ---------- Pulso ----------
static void generate_pulse(int gpio, const char *label, int count, int interval_ms, int pulse_duration_ms) {
    gpio_set_level(gpio, 0);
    vTaskDelay(pdMS_TO_TICKS(pulse_duration_ms));
    gpio_set_level(gpio, 1);
    
    ESP_LOGI(LOG_TAG, "%s | Pulso %d | Duração: %d ms | Próximo em: %d ms", 
             label, count, pulse_duration_ms, interval_ms);
}

static void pulse_task(void *pvParameter) {
    pulse_config_t *config = (pulse_config_t *)pvParameter;
    int count = 0;

    if (!config) {
        ESP_LOGE(LOG_TAG, "Configuração inválida!");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(LOG_TAG, "%s iniciado | Modo: %s | Intervalo base: %d ms | Pulse: %d ms",
             config->label,
             config->mode == MODE_DEFINED ? "Definido" : "Aleatório",
             config->interval_ms,
             config->pulse_duration_ms);

    gpio_set_level(config->gpio, 1);

    while (1) {
        int interval = (config->mode == MODE_RANDOM)
                       ? (esp_random() % config->interval_ms) + 1
                       : config->interval_ms;

        if (interval <= config->pulse_duration_ms) {
            interval = config->pulse_duration_ms + 1;
        }

        generate_pulse(config->gpio, config->label, ++count, interval, config->pulse_duration_ms);

        int remaining_ms = interval - config->pulse_duration_ms;
        if (remaining_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(remaining_ms));
        }
        
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// Função para configurar uma saída individual
static pulse_config_t* configure_single_output(int output_number, int gpio) {
    printf("\n=== Configuração Saída %d (GPIO%d) ===\n", output_number, gpio);
    
    int interval = read_int_from_uart("Intervalo entre pulsos", MIN_INTERVAL_MS, MAX_INTERVAL_MS);
    if (interval < 0) {
        printf("Intervalo inválido para saída %d!\n", output_number);
        return NULL;
    }

    int pulse_duration = read_int_from_uart("Duração do pulso", MIN_PULSE_MS, MAX_PULSE_MS);
    if (pulse_duration < 0) {
        printf("Duração de pulso inválida para saída %d!\n", output_number);
        return NULL;
    }

    if (pulse_duration >= interval) {
        printf("ERRO: A duração do pulso (%d ms) deve ser MENOR que o intervalo (%d ms)!\n", 
               pulse_duration, interval);
        printf("Recomendações:\n");
        printf(" - Para intervalo de %d ms, o pulso deve ser no máximo %d ms\n", interval, interval-1);
        return NULL;
    }

    pulse_config_t *config = malloc(sizeof(pulse_config_t));
    if (!config) {
        printf("Erro de memória!\n");
        return NULL;
    }
    
    config->gpio = gpio;
    config->interval_ms = interval;
    config->pulse_duration_ms = pulse_duration;
    config->mode = select_mode(interval);
    config->label = (output_number == 1) ? "Saída 1" : "Saída 2";
    
    return config;
}

void app_main(void) {
    configure_uart();
    
    esp_log_level_set("*", ESP_LOG_ERROR);
    esp_log_level_set(LOG_TAG, ESP_LOG_INFO);
    setvbuf(stdout, NULL, _IONBF, 0);

    int num_outputs = ask_number_of_outputs();
    
    pulse_config_t *config1 = NULL;
    pulse_config_t *config2 = NULL;

    printf("\n");
    config1 = configure_single_output(1, GPIO_OUT_1);
    if (!config1) {
        printf("Falha na configuração da Saída 1!\n");
        return;
    }

    if (num_outputs == 2) {
        config2 = configure_single_output(2, GPIO_OUT_2);
        if (!config2) {
            printf("Falha na configuração da Saída 2!\n");
            free(config1);
            return;
        }
    }

    printf("\n=== Resumo da Configuração ===\n");
    printf("Saída 1: GPIO=%d, Intervalo=%d ms, Pulso=%d ms, Modo=%s\n", 
           config1->gpio, config1->interval_ms, config1->pulse_duration_ms,
           config1->mode == MODE_DEFINED ? "Definido" : "Aleatório");
    
    if (config2) {
        printf("Saída 2: GPIO=%d, Intervalo=%d ms, Pulso=%d ms, Modo=%s\n", 
               config2->gpio, config2->interval_ms, config2->pulse_duration_ms,
               config2->mode == MODE_DEFINED ? "Definido" : "Aleatório");
    } else {
        printf("Saída 2: Não utilizada\n");
    }

    configure_gpio(GPIO_OUT_1);
    if (config2) {
        configure_gpio(GPIO_OUT_2);
    }

    wait_for_start();

    xTaskCreate(pulse_task, "pulse_task_1", 4096, config1, 5, NULL);
    if (config2) {
        xTaskCreate(pulse_task, "pulse_task_2", 4096, config2, 5, NULL);
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}