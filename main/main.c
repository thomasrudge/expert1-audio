#include <FreeRTOS.h>
#include <task.h>
#include <semphr.h>
#include <queue.h>


#include <math.h>
#include <stdio.h>
#include "pico/stdlib.h"   // stdlib 
#include "hardware/irq.h"  // interrupts
#include "hardware/pwm.h"  // pwm
#include "hardware/sync.h" // wait for interrupt
#include "hardware/pll.h"
#include "hardware/clocks.h"
#include "hardware/adc.h"  // adc
#include <stdlib.h>
#include "ring.h"

#define AUDIO_OUT_PIN 28
#define AUDIO_IN_PIN 27

#define SAMPLE_RATE 16000
#define DATA_LENGTH SAMPLE_RATE*4 // WAV_DATA_LENGTH //16000
#define FREQ 8000

char audio[DATA_LENGTH];

int wav_position = 0;

SemaphoreHandle_t xSemaphorePlayInit;
SemaphoreHandle_t xSemaphorePlayDone;
SemaphoreHandle_t xSemaphoreRecordDone;

int audio_pin_slice;

/*
 * PWM Interrupt Handler which outputs PWM level and advances the
 * current sample.
 *
 * We repeat the same value for 8 cycles this means sample rate etc
 * adjust by factor of 8 (this is what bitshifting <<3 is doing)
 *
 */
void pwm_interrupt_handler() {
    pwm_clear_irq(pwm_gpio_to_slice_num(AUDIO_OUT_PIN));
    if (wav_position < (DATA_LENGTH << 3) - 1) {
        // set pwm level
        // allow the pwm value to repeat for 8 cycles this is >>3
        pwm_set_gpio_level(AUDIO_OUT_PIN, audio[wav_position >> 3]);
        wav_position++;
    }
    else {
        // Acabou de reproduzir o áudio
        // wav_position = 0;
        xSemaphoreGiveFromISR(xSemaphorePlayDone, 0);
    }
}

bool fala_detectada = false;
int valor_medio = 120;        // valor típico de silêncio (ajustamos depois)
int limite = 50;              // quanto acima/abaixo precisa variar para considerar fala

bool aguardando_fala = true;  // ficou esperando alguém falar
bool gravando = false;        // só vira true quando fala começar
int contagem_amostras = 0;    // conta quantas amostras gravamos após a fala

float filtro = 120;   // começa no valor médio




bool timer_0_callback(repeating_timer_t* rt) {

    uint16_t leitura = adc_read() / 16;

    // -----------------------
    // 1. ESPERANDO ALGUÉM FALAR
    // -----------------------
    if (aguardando_fala) {
        //printf("%d\n", leitura);

        if (abs(leitura - valor_medio) > limite) {
            fala_detectada = true;
            aguardando_fala = false;
            gravando = true;
            wav_position = 0;          // começa a gravar do 0
            contagem_amostras = 0;
            printf("Fala detectada! Gravando...\n");
        }

        return true;
    }

    // -----------------------
    // 2. GRAVANDO POR 4s
    // -----------------------
    if (gravando) {

        if (wav_position >= DATA_LENGTH) {
        gravando = false;
        xSemaphoreGiveFromISR(xSemaphoreRecordDone, 0);
        return false;
    }

        filtro = filtro * 0.85 + leitura * 0.15;

        // --- AUMENTO DE VOLUME ---
        float ampliado = filtro * 1.6;   // ajuste 1.2 ~ 2.0 conforme quiser

        // limitar para não estourar o char (0–255)
        if (ampliado < 0) ampliado = 0;
        if (ampliado > 255) ampliado = 255;

        audio[wav_position++] = (char)ampliado;

        contagem_amostras++;

        //printf("%d\n", leitura);

        // terminou 4 segundos?
        if (contagem_amostras >= DATA_LENGTH) {
            printf("Amostras gravadas: %d (deveria ser %d)\n", contagem_amostras, DATA_LENGTH);
            gravando = false;

            printf("Gravacao finalizada! Pronto pra reproduzir.\n");

            // avisar a task de play
            xSemaphoreGiveFromISR(xSemaphoreRecordDone, 0);

            return false; // para o timer
        }

        return true;
}


    return true;
}


void sin_task() {
    int freq = 1000;
    for (int i = 0; i < SAMPLE_RATE; i++) {
        float sineValue = sin(2.0 * M_PI * freq * i / SAMPLE_RATE);
        int sample = (int)(((sineValue + 1.0) / 2.0) * 255.0);
        audio[i] = (char)sample;
    }

    vTaskDelay(100);
    xSemaphoreGive(xSemaphorePlayInit);

    while (1) {
    }
}


void mic_task() {
    adc_gpio_init(AUDIO_IN_PIN);
    adc_init();
    adc_select_input(AUDIO_IN_PIN - 26);

    /**
    * clkdiv should be as follows for given sample rate
    *  8.0f for 11 KHz
    *  4.0f for 22 KHz
    *  2.0f for 44 KHz etc
    */
    int timer_0_hz = SAMPLE_RATE; 
    repeating_timer_t timer_0;

    while (1) {

        aguardando_fala = true;
        gravando = false;
        fala_detectada = false;

        wav_position = 0;

        if (!add_repeating_timer_us(1000000 / timer_0_hz,
            timer_0_callback,
            NULL,
            &timer_0)) {
            printf("Failed to add timer\n");
        }

        if (xSemaphoreTake(xSemaphoreRecordDone, portMAX_DELAY) == pdTRUE) {
            cancel_repeating_timer(&timer_0);
        }

        xSemaphoreGive(xSemaphorePlayInit);

        


        if (xSemaphoreTake(xSemaphorePlayDone, portMAX_DELAY) == pdTRUE) {
            pwm_set_enabled(audio_pin_slice, false);
        }
    }
}

void play_task() {
    /*
     * Based on: https://github.com/rgrosset/pico-pwm-audio/
     * Overclocking for fun but then also so the system clock is a
     * multiple of typical audio sampling rates.
     */
    gpio_set_function(AUDIO_OUT_PIN, GPIO_FUNC_PWM);

    audio_pin_slice = pwm_gpio_to_slice_num(AUDIO_OUT_PIN);

    // Setup PWM interrupt to fire when PWM cycle is complete
    pwm_clear_irq(audio_pin_slice);
    pwm_set_irq_enabled(audio_pin_slice, true);
    irq_set_exclusive_handler(PWM_IRQ_WRAP, pwm_interrupt_handler);
    irq_set_enabled(PWM_IRQ_WRAP, true);

    pwm_config config = pwm_get_default_config();

    /* Base clock 176,000 Hz divide by wrap 250 then the clock divider further divides
     * to set the interrupt rate.
     *
     * 11 KHz is fine for speech. Phone lines generally sample at 8 KHz
     *
     *
     * So clkdiv should be as follows for given sample rate
     *  8.0f for 11 KHz
     *  4.0f for 22 KHz
     *  2.0f for 44 KHz etc
     */
    float pwm_clkdiv = 4.0f; // aproximadamente 11 kHz
    uint16_t wrap = 250;

    pwm_config_set_clkdiv(&config, pwm_clkdiv);
    pwm_config_set_wrap(&config, wrap);

    while (1) {
        if (xSemaphoreTake(xSemaphorePlayInit, pdMS_TO_TICKS(500)) == pdTRUE) {
            wav_position = 0;
            pwm_init(audio_pin_slice, &config, true);
            pwm_set_gpio_level(AUDIO_OUT_PIN, 0);
        }

        // if (xSemaphoreTake(xSemaphorePlayDone, pdMS_TO_TICKS(500)) == pdTRUE) {
        //     pwm_set_enabled(audio_pin_slice, false);
        // }
    }
}

int main() {
    stdio_init_all();
    printf("oi\n");

    xSemaphorePlayInit = xSemaphoreCreateBinary();
    xSemaphorePlayDone = xSemaphoreCreateBinary();
    xSemaphoreRecordDone = xSemaphoreCreateBinary();

    xTaskCreate(play_task, "Play Task", 4095, NULL, 1, NULL);
    xTaskCreate(mic_task, "Mic Task", 4095, NULL, 1, NULL);

    vTaskStartScheduler();

    while (true)
        ;
}
