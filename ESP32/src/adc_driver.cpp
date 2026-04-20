#include "adc_driver.h"
#include "driver/adc.h"

static uint16_t raw_ch1[ADC_BUFFER_SIZE];
static uint16_t raw_ch2[ADC_BUFFER_SIZE];
static int32_t ch1_buffer[ADC_SAMPLE_COUNT];
static int32_t ch2_buffer[ADC_SAMPLE_COUNT];
static volatile bool data_ready = false;
static volatile adc_trigger_mode_t trig_mode = ADC_TRIG_RISING;

void adc_driver_set_trigger(adc_trigger_mode_t mode) { trig_mode = mode; }

static uint16_t median3(adc1_channel_t ch) {
    uint16_t a = adc1_get_raw(ch);
    uint16_t b = adc1_get_raw(ch);
    uint16_t c = adc1_get_raw(ch);
    if (a > b) { uint16_t t = a; a = b; b = t; }
    if (b > c) { uint16_t t = b; b = c; c = t; }
    if (a > b) { uint16_t t = a; a = b; b = t; }
    return b;
}

void adc_sampling_task(void *param) {
    while (true) {
        if (data_ready) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // 1. 以固定间隔填满双倍缓冲区（中位数滤波去毛刺）
        for (int i = 0; i < ADC_BUFFER_SIZE; i++) {
            raw_ch2[i] = adc1_get_raw(ADC1_CHANNEL_4);
        }

        // 2. 动态计算触发电平（用CH2）
        uint16_t ch2_max = 0, ch2_min = 4095;
        for (int i = 0; i < ADC_BUFFER_SIZE; i++) {
            if (raw_ch2[i] > ch2_max) ch2_max = raw_ch2[i];
            if (raw_ch2[i] < ch2_min) ch2_min = raw_ch2[i];
        }
        uint16_t dyn_trigger = (ch2_max + ch2_min) / 2;

        int trigger = -1;
        if (trig_mode == ADC_TRIG_NONE) {
            trigger = 0;
        } else {
            for (int i = 1; i < ADC_BUFFER_SIZE / 2; i++) {
                if (trig_mode == ADC_TRIG_RISING &&
                    raw_ch2[i-1] < dyn_trigger && raw_ch2[i] >= dyn_trigger) {
                    trigger = i; break;
                } else if (trig_mode == ADC_TRIG_FALLING &&
                    raw_ch2[i-1] >= dyn_trigger && raw_ch2[i] < dyn_trigger) {
                    trigger = i; break;
                }
            }
            if (trigger < 0) trigger = 0;
        }

        // 3. 找第二个触发点，计算周期，动态步长覆盖3个周期
        int step = 10; // 默认步长
        if (trigger > 0 && trig_mode != ADC_TRIG_NONE) {
            int trigger2 = -1;
            for (int i = trigger + 2; i < ADC_BUFFER_SIZE - 1; i++) {
                if (trig_mode == ADC_TRIG_RISING &&
                    raw_ch2[i-1] < dyn_trigger && raw_ch2[i] >= dyn_trigger) {
                    trigger2 = i; break;
                } else if (trig_mode == ADC_TRIG_FALLING &&
                    raw_ch2[i-1] >= dyn_trigger && raw_ch2[i] < dyn_trigger) {
                    trigger2 = i; break;
                }
            }
            if (trigger2 > trigger) {
                int period = trigger2 - trigger;
                // 步长让200个点覆盖3个周期
                step = (period * 3) / ADC_SAMPLE_COUNT;
                if (step < 1) step = 1;
            }
        }

        for (int i = 0; i < ADC_SAMPLE_COUNT; i++) {
            int idx = trigger + i * step;
            if (idx >= ADC_BUFFER_SIZE) idx = ADC_BUFFER_SIZE - 1;
            ch1_buffer[i] = 0;
            ch2_buffer[i] = ((int32_t)raw_ch2[idx] - dyn_trigger) * 2;
        }

        data_ready = true;

        Serial.printf("CH2 min=%d max=%d trig=%d\n", ch2_min, ch2_max, dyn_trigger);

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void adc_driver_init() {
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(ADC1_CHANNEL_3, ADC_ATTEN_DB_11);
    adc1_config_channel_atten(ADC1_CHANNEL_4, ADC_ATTEN_DB_11);
    xTaskCreatePinnedToCore(adc_sampling_task, "adc_task", 4096, NULL, 1, NULL, 0);
}

void adc_driver_start() { data_ready = false; }
bool adc_driver_is_ready() { return data_ready; }

void adc_driver_get_data(int32_t* ch1_buf, int32_t* ch2_buf, uint32_t count) {
    if (!data_ready || count > ADC_SAMPLE_COUNT) return;
    for (uint32_t i = 0; i < count; i++) {
        ch1_buf[i] = ch1_buffer[i];
        ch2_buf[i] = ch2_buffer[i];
    }
    data_ready = false;
}
