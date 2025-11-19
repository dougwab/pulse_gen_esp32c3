#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_random.h"

// ========== CONFIGURAÇÕES SIMPLIFICADAS ==========
#define GPIO_OUT_1          4
#define GPIO_OUT_2          5
#define LOG_TAG             "PULSE_GEN"
#define MAX_PPS             1000
#define MIN_PPS             1
#define MAX_INTERVAL_MS     3600000
#define MIN_INTERVAL_MS     (1000 / MAX_PPS)
#define MIN_PULSE_MS        1
#define MAX_PULSE_MS        10000
#define UART_BUFFER_SIZE    1024
#define UART_PORT           UART_NUM_0
#define UART_BAUD_RATE      115200
#define MIN_SAFE_INTERVAL_MS 2

// ========== TIPOS ==========
typedef enum {
    MODE_DEFINED,
    MODE_RANDOM
} pulse_mode_t;

typedef enum {
    STATE_RUNNING,
    STATE_PAUSED,
    STATE_STOPPED
} generator_state_t;

typedef struct {
    int gpio;
    int interval_ms;
    int pulse_duration_ms;
    pulse_mode_t mode;
    const char *label;
    int max_pulses;
    int pps;
    generator_state_t state;
    int pulse_count;
} pulse_config_t;

// ========== VARIÁVEIS GLOBAIS ==========
static pulse_config_t active_configs[2];
static int active_outputs = 0;
static volatile bool system_running = false;
static volatile bool pause_requested = false;

// ========== IMPLEMENTAÇÃO ==========

static void configure_uart(void) {
    uart_config_t uart_config = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, 
                                UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, 256, 0, 0, NULL, 0));
}

static void configure_gpio(int gpio) {
    gpio_reset_pin(gpio);
    gpio_set_direction(gpio, GPIO_MODE_OUTPUT);
    gpio_set_level(gpio, 1);
}

static char uart_read_char(void) {
    uint8_t data;
    while (uart_read_bytes(UART_PORT, &data, 1, 100 / portTICK_PERIOD_MS) <= 0) {
    }
    return (char)data;
}

static int read_int_from_uart(const char *prompt, int min_val, int max_val) {
    char input[12] = {0};
    int index = 0;
    char c;
    
    printf("\n%s (%d a %d): ", prompt, min_val, max_val);
    
    while (index < 11) {
        c = uart_read_char();
        if (c == '\r' || c == '\n') {
            if (index > 0) break;
        } else if (c == 8 || c == 127) {
            if (index > 0) {
                index--;
                printf("\b \b");
            }
        } else if (c >= '0' && c <= '9') {
            input[index++] = c;
            putchar(c);
        }
    }
    printf("\n");
    
    if (index == 0) {
        return -1;
    }
    
    int value = atoi(input);
    if (value < min_val || value > max_val) {
        printf("Valor inválido! Use entre %d e %d.\n", min_val, max_val);
        return -1;
    }
    return value;
}

static void print_header(void) {
    printf("\n");
    printf("========================================\n");
    printf("          GERADOR DE PULSOS\n");
    printf("             DABSTACK\n");
    printf("========================================\n");
}

static int ask_number_of_outputs(void) {
    printf("\n--- CONFIGURAÇÃO DE SAÍDAS ---\n");
    printf("1. Saída 1 (GPIO4)\n");
    printf("2. Saída 2 (GPIO5)\n");
    printf("3. Ambas saídas\n");
    printf("Escolha (1-3): ");

    char c = uart_read_char();
    printf("%c\n", c);
    
    return (c >= '1' && c <= '3') ? (c - '0') : 1;
}

static int ask_pps_config(void) {
    printf("\n--- TIPO DE CONFIGURAÇÃO ---\n");
    printf("I. Intervalo entre pulsos (ms)\n");
    printf("P. Pulsos por segundo (PPS)\n");
    printf("Escolha (I/P): ");

    char c = uart_read_char();
    printf("%c\n", c);
    
    if (c == 'P' || c == 'p') {
        int pps = read_int_from_uart("Pulsos por segundo", MIN_PPS, MAX_PPS);
        if (pps < 0) return -1;
        
        int interval_ms = 1000 / pps;
        printf(">> %d PPS = %d ms entre pulsos\n", pps, interval_ms);
        return interval_ms;
    } else {
        return read_int_from_uart("Intervalo entre pulsos (ms)", MIN_INTERVAL_MS, MAX_INTERVAL_MS);
    }
}

static pulse_mode_t select_mode(void) {
    printf("\n--- MODO DE OPERAÇÃO ---\n");
    printf("D. Intervalo fixo\n");
    printf("R. Intervalo aleatório\n");
    printf("Escolha (D/R): ");

    char c = uart_read_char();
    printf("%c\n", c);
    
    return (c == 'D' || c == 'd') ? MODE_DEFINED : MODE_RANDOM;
}

static int ask_pulse_limit(void) {
    printf("\n--- LIMITE DE PULSOS ---\n");
    printf("S. Com limite\n");
    printf("N. Sem limite (contínuo)\n");
    printf("Escolha (S/N): ");

    char c = uart_read_char();
    printf("%c\n", c);
    
    if (c == 'S' || c == 's') {
        return read_int_from_uart("Quantidade de pulsos", 1, 1000000);
    }
    return 0;
}

static void generate_pulse(int gpio, int pulse_duration_ms) {
    gpio_set_level(gpio, 0);
    vTaskDelay(pdMS_TO_TICKS(pulse_duration_ms));
    gpio_set_level(gpio, 1);
}

// SISTEMA DE PAUSA/RETOMADA
static void handle_pause_system(void) {
    pause_requested = !pause_requested;
    
    if (pause_requested) {
        printf("\n>> SISTEMA PAUSADO - Espaço para retomar\n");
        for (int i = 0; i < active_outputs; i++) {
            active_configs[i].state = STATE_PAUSED;
        }
    } else {
        printf("\n>> SISTEMA RETOMADO\n");
        for (int i = 0; i < active_outputs; i++) {
            active_configs[i].state = STATE_RUNNING;
        }
    }
}

// TASK DE PULSO CORRIGIDA - MOSTRA TODOS OS PULSOS
static void pulse_task(void *pvParameter) {
    pulse_config_t *config = (pulse_config_t *)pvParameter;
    uint32_t last_pulse_time = 0;
    
    if (!config) {
        vTaskDelete(NULL);
        return;
    }

    // Configuração inicial
    gpio_set_level(config->gpio, 1);
    config->state = STATE_RUNNING;
    config->pulse_count = 0;
    
    ESP_LOGI(LOG_TAG, "%s INICIADO | %d PPS | %d ms pulse | Max: %s", 
             config->label, config->pps, config->pulse_duration_ms,
             config->max_pulses == 0 ? "Infinito" : "");

    // Loop principal
    while (system_running && (config->max_pulses == 0 || config->pulse_count < config->max_pulses)) {
        // Verificar se está pausado
        if (config->state == STATE_PAUSED) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        
        uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
        
        // Verifica se é hora do próximo pulso
        if (current_time - last_pulse_time >= config->interval_ms) {
            // Log ANTES de gerar o pulso para garantir que aparece
            ESP_LOGI(LOG_TAG, "%s | Pulse %d", config->label, config->pulse_count + 1);
            
            generate_pulse(config->gpio, config->pulse_duration_ms);
            config->pulse_count++;
            last_pulse_time = current_time;
        }
        
        vTaskDelay(pdMS_TO_TICKS(1)); // Delay mínimo para não sobrecarregar
    }

    // Finalização - REMOVIDO O LOG AQUI
    config->state = STATE_STOPPED;
    
    // Sinalização visual de fim
    for (int i = 0; i < 3; i++) {
        gpio_set_level(config->gpio, 0);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(config->gpio, 1);
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    vTaskDelete(NULL);
}

static bool configure_output(int output_num, int gpio) {
    printf("\n--- SAÍDA %d (GPIO%d) ---\n", output_num, gpio);
    
    pulse_config_t *config = &active_configs[output_num - 1];
    
    // Configuração básica
    config->gpio = gpio;
    config->label = (output_num == 1) ? "OUT1" : "OUT2";
    
    // Obter parâmetros
    config->interval_ms = ask_pps_config();
    if (config->interval_ms < 0) {
        return false;
    }
    
    config->pulse_duration_ms = read_int_from_uart("Duração do pulso (ms)", 
                                                  MIN_PULSE_MS, MAX_PULSE_MS);
    if (config->pulse_duration_ms < 0) {
        return false;
    }
    
    config->mode = select_mode();
    config->max_pulses = ask_pulse_limit();
    config->pps = 1000 / config->interval_ms;
    config->state = STATE_STOPPED;
    config->pulse_count = 0;
    
    return true;
}

void app_main(void) {
    // Configuração inicial
    configure_uart();
    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set(LOG_TAG, ESP_LOG_INFO);
    setvbuf(stdout, NULL, _IONBF, 0);

    while (1) {
        system_running = false;
        pause_requested = false;
        print_header();
        
        // Configuração
        int num_outputs = ask_number_of_outputs();
        active_outputs = (num_outputs == 3) ? 2 : num_outputs;
        
        bool config_success = true;
        
        // Configurar saídas
        for (int i = 0; i < active_outputs; i++) {
            if (!configure_output(i + 1, (i == 0) ? GPIO_OUT_1 : GPIO_OUT_2)) {
                config_success = false;
                break;
            }
            configure_gpio(active_configs[i].gpio);
        }

        if (!config_success) {
            printf("Erro na configuração! Reiniciando...\n");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        // Resumo
        printf("\n--- RESUMO ---\n");
        for (int i = 0; i < active_outputs; i++) {
            printf("%s: %d PPS, %d ms pulse, %s\n", 
                   active_configs[i].label, active_configs[i].pps, 
                   active_configs[i].pulse_duration_ms,
                   active_configs[i].max_pulses == 0 ? "Contínuo" : "Limitado");
        }

        // Confirmação
        printf("\nPressione ENTER para iniciar, C para cancelar: ");
        char start_cmd = uart_read_char();
        printf("%c\n", start_cmd);
        
        if (start_cmd == 'C' || start_cmd == 'c') {
            continue;
        }

        printf("\n>> INICIANDO GERADOR...\n");
        printf(">> BARRA DE ESPAÇO: Pausar/Retomar\n");
        printf("========================================\n");

        system_running = true;

        // Iniciar tasks de pulso
        for (int i = 0; i < active_outputs; i++) {
            const char* task_names[] = {"pulse_1", "pulse_2"};
            xTaskCreate(pulse_task, task_names[i], 4096, &active_configs[i], 3, NULL);
        }

        // Loop principal de monitoramento
        bool tasks_running = true;
        while (tasks_running && system_running) {
            // Verificar comando de pausa
            uint8_t data;
            if (uart_read_bytes(UART_PORT, &data, 1, 100 / portTICK_PERIOD_MS) > 0) {
                char cmd = (char)data;
                if (cmd == ' ') {
                    handle_pause_system();
                }
            }
            
            // Verificar se as tasks ainda estão rodando
            tasks_running = false;
            for (int i = 0; i < active_outputs; i++) {
                if (active_configs[i].state != STATE_STOPPED) {
                    tasks_running = true;
                    break;
                }
            }
            
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        // Finalização - ORDEM CORRIGIDA DOS LOGS
        system_running = false;
        printf("\n>> GERADOR FINALIZADO\n");
        
        // Agora mostra o resumo de pulsos gerados
        for (int i = 0; i < active_outputs; i++) {
            ESP_LOGI(LOG_TAG, "%s FINALIZADO | %d pulsos gerados", 
                     active_configs[i].label, active_configs[i].pulse_count);
        }
        
        printf(">> Reiniciando em 2 segundos...\n");
        vTaskDelay(pdMS_TO_TICKS(2000));
        
        // Aguardar tasks finalizarem
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}