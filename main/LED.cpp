#include <sys/param.h>

#include "LED.h"

void LED_task (void *parameters)
{
    LED *led = (LED *) parameters;

    while (true)
    {
        bool output = false;

        if (xSemaphoreTake (led->_semaphore, portMAX_DELAY))
        {
            if (led->_msec > 0)
            {
                led->_msec = MAX (0, led->_msec - led->_tick);
                output = true;
            }

            xSemaphoreGive (led->_semaphore);
        }

        gpio_set_level (led->_pin, output);

        vTaskDelay (led->_tick / portTICK_PERIOD_MS);
    }
}

LED::LED (gpio_num_t pin, int msec)
{
    _pin = pin;
    _msec = msec;
    _tick = 10;

    // configure the LED
    gpio_config_t io_conf;

    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << _pin);
    io_conf.pull_down_en = (gpio_pulldown_t) 0;
    io_conf.pull_up_en = (gpio_pullup_t) 0;

    gpio_config (&io_conf);

    // create and take the semaphore
    _semaphore = xSemaphoreCreateBinary ();

    // start the LED control task
    _task = NULL;
    xTaskCreate (LED_task, "LED_task", 4096, (void *) this, tskIDLE_PRIORITY, &_task);

    // release the semaphore
    xSemaphoreGive (_semaphore);
}

LED::~LED ()
{
    // delete the task
    vTaskDelete (_task);

    // delete the semaphore
    vSemaphoreDelete (_semaphore);
}

void LED::on (int msec)
{
    _msec = _msec + msec;
}

void LED::off (void)
{
    _msec = 0;
}
