#ifndef __ROBOT_H__
#define __ROBOT_H__

#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "LED.h"

// -----
// robot
// -----
#define FRONT_MOTOR     (0)
#define LEFT_MOTOR      (1)
#define RIGHT_MOTOR     (2)

class Robot
{
    public:
        Robot (std::string name, LED *led = NULL);
        ~Robot ();
        std::string  name (void);
    public:
        bool connected (void);
        bool connected (bool state, uint32_t handle);
        void battery (void);
        void keepalive (void);
        void beep (void);
        void motor (uint8_t port, int8_t speed);
        bool process (uint32_t handle, uint16_t length, uint8_t *data);
        std::string status (void);
    public:
        void command (uint8_t *data);
    public:
        SemaphoreHandle_t _semaphore;
        TaskHandle_t _task;
        int8_t _output[3];
    private:
        std::string _name;
        LED *_led;
        bool _connected;
        uint32_t _handle;
        float _battery;
        float _keepalive;
};

#endif
