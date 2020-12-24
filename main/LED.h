#ifndef __LED_H__
#define __LED_H__

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

class LED
{
    public:
        LED (gpio_num_t pin, int count = 0);
        ~LED ();
        void on (int msec = 10);
        void off (void);
    public:
        SemaphoreHandle_t _semaphore;
        TaskHandle_t _task;
        gpio_num_t _pin;
        int _msec;
};

#endif
