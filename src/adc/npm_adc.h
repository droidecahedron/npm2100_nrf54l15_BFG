#ifndef NPM_ADC_H_
#define NPM_ADC_H_

#include <stdint.h>
struct adc_sample_msg
{
    int32_t channel_mv[2];
};

#endif