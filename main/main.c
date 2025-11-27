#include <FreeRTOS.h>
#include <task.h>
#include <semphr.h>
#include <queue.h>

#include <math.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/irq.h"
#include "hardware/pwm.h"
#include "hardware/sync.h"
#include "hardware/pll.h"
#include "hardware/clocks.h"
#include "hardware/adc.h"
#include <stdlib.h>
#include "ring.h"

#define AUDIO_OUT_PIN 28
#define AUDIO_IN_PIN 27
#define SAMPLE_RATE 16000
#define DATA_LENGTH SAMPLE_RATE*4
#define FREQ 8000

typedef struct {
    volatile char audio[DATA_LENGTH];
    volatile int wav_position;
    volatile bool fala_detectada;
    volatile bool aguardando_fala;
    volatile bool gravando;
    volatile int contagem_amostras;
    volatile float filtro;
    int audio_pin_slice;
    int valor_medio;
    int limite;
    SemaphoreHandle_t xSemaphorePlayInit;
    SemaphoreHandle_t xSemaphorePlayDone;
    SemaphoreHandle_t xSemaphoreRecordDone;
} AudioSystem_t;

// Define como endereço fixo de memória ao invés de variável global
#define g_audio_ptr ((volatile AudioSystem_t*)0x20040000)

void pwm_interrupt_handler() {
    pwm_clear_irq(pwm_gpio_to_slice_num(AUDIO_OUT_PIN));
    if (g_audio_ptr->wav_position < (DATA_LENGTH << 3) - 1) {
        pwm_set_gpio_level(AUDIO_OUT_PIN, g_audio_ptr->audio[g_audio_ptr->wav_position >> 3]);
        g_audio_ptr->wav_position++;
    }
    else {
        xSemaphoreGiveFromISR(g_audio_ptr->xSemaphorePlayDone, 0);
    }
}

bool timer_0_callback(repeating_timer_t* rt) {
    AudioSystem_t* sys = (AudioSystem_t*)rt->user_data;
    uint16_t leitura = adc_read() / 16;

    if (sys->aguardando_fala) {
        if (abs(leitura - sys->valor_medio) > sys->limite) {
            sys->fala_detectada = true;
            sys->aguardando_fala = false;
            sys->gravando = true;
            sys->wav_position = 0;
            sys->contagem_amostras = 0;
        }
        return true;
    }

    if (sys->gravando) {
        if (sys->wav_position >= DATA_LENGTH) {
            sys->gravando = false;
            xSemaphoreGiveFromISR(sys->xSemaphoreRecordDone, 0);
            return false;
        }

        sys->filtro = sys->filtro * 0.85 + leitura * 0.15;
        float ampliado = sys->filtro * 1.6;

        if (ampliado < 0) ampliado = 0;
        if (ampliado > 255) ampliado = 255;

        sys->audio[sys->wav_position++] = (char)ampliado;
        sys->contagem_amostras++;

        if (sys->contagem_amostras >= DATA_LENGTH) {
            sys->gravando = false;
            xSemaphoreGiveFromISR(sys->xSemaphoreRecordDone, 0);
            return false;
        }
        return true;
    }

    return true;
}

void mic_task(void* params) {
    AudioSystem_t* sys = (AudioSystem_t*)params;
    
    adc_gpio_init(AUDIO_IN_PIN);
    adc_init();
    adc_select_input(AUDIO_IN_PIN - 26);

    int timer_0_hz = SAMPLE_RATE;
    repeating_timer_t timer_0;

    while (1) {
        sys->aguardando_fala = true;
        sys->gravando = false;
        sys->fala_detectada = false;
        sys->wav_position = 0;

        if (!add_repeating_timer_us(1000000 / timer_0_hz,
            timer_0_callback,
            sys,
            &timer_0)) {
            printf("Failed to add timer\n");
        }

        if (xSemaphoreTake(sys->xSemaphoreRecordDone, portMAX_DELAY) == pdTRUE) {
            cancel_repeating_timer(&timer_0);
        }

        xSemaphoreGive(sys->xSemaphorePlayInit);

        if (xSemaphoreTake(sys->xSemaphorePlayDone, portMAX_DELAY) == pdTRUE) {
            pwm_set_enabled(sys->audio_pin_slice, false);
        }
    }
}

void play_task(void* params) {
    AudioSystem_t* sys = (AudioSystem_t*)params;
    
    gpio_set_function(AUDIO_OUT_PIN, GPIO_FUNC_PWM);
    sys->audio_pin_slice = pwm_gpio_to_slice_num(AUDIO_OUT_PIN);

    pwm_clear_irq(sys->audio_pin_slice);
    pwm_set_irq_enabled(sys->audio_pin_slice, true);
    irq_set_exclusive_handler(PWM_IRQ_WRAP, pwm_interrupt_handler);
    irq_set_enabled(PWM_IRQ_WRAP, true);

    pwm_config config = pwm_get_default_config();
    float pwm_clkdiv = 4.0f;
    uint16_t wrap = 250;

    pwm_config_set_clkdiv(&config, pwm_clkdiv);
    pwm_config_set_wrap(&config, wrap);

    while (1) {
        if (xSemaphoreTake(sys->xSemaphorePlayInit, pdMS_TO_TICKS(500)) == pdTRUE) {
            sys->wav_position = 0;
            pwm_init(sys->audio_pin_slice, &config, true);
            pwm_set_gpio_level(AUDIO_OUT_PIN, 0);
        }
    }
}

int main() {
    stdio_init_all();
    printf("oi\n");

    // Aloca em endereço fixo de RAM
    volatile AudioSystem_t* audio_system = (volatile AudioSystem_t*)0x20040000;
    
    audio_system->wav_position = 0;
    audio_system->fala_detectada = false;
    audio_system->aguardando_fala = true;
    audio_system->gravando = false;
    audio_system->contagem_amostras = 0;
    audio_system->filtro = 120;
    audio_system->valor_medio = 120;
    audio_system->limite = 50;

    audio_system->xSemaphorePlayInit = xSemaphoreCreateBinary();
    audio_system->xSemaphorePlayDone = xSemaphoreCreateBinary();
    audio_system->xSemaphoreRecordDone = xSemaphoreCreateBinary();

    xTaskCreate(play_task, "Play Task", 4095, (void*)audio_system, 1, NULL);
    xTaskCreate(mic_task, "Mic Task", 4095, (void*)audio_system, 1, NULL);

    vTaskStartScheduler();

    while (true);
}