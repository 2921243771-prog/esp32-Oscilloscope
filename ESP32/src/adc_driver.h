#ifndef ADC_DRIVER_H
#define ADC_DRIVER_H

#include <Arduino.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ADC_CH1_PIN         4
#define ADC_CH2_PIN         5
#define ADC_SAMPLE_COUNT    200
#define ADC_BUFFER_SIZE     2000

typedef enum {
    ADC_TRIG_NONE = 0,
    ADC_TRIG_RISING,
    ADC_TRIG_FALLING
} adc_trigger_mode_t;

void adc_driver_init();
void adc_driver_start();
bool adc_driver_is_ready();
void adc_driver_get_data(int32_t* ch1_buf, int32_t* ch2_buf, uint32_t count);
void adc_driver_set_trigger(adc_trigger_mode_t mode);

#ifdef __cplusplus
}
#endif

#endif
