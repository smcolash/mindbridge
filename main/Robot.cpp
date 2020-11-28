#include <string>

#include "esp_spp_api.h"

#include "Robot.h"

void Robot_task (void *parameters)
{
    Robot *robot = (Robot *) parameters;

    int8_t output[3];
    output[0] = 0;
    output[1] = 0;
    output[2] = 0;

    while (true)
    {
        if (xSemaphoreTake (robot->_semaphore, portMAX_DELAY))
        {
            int8_t pending[3];

            for (int loop = 0; loop < 3; loop++)
            {
                pending[loop] = robot->_output[loop];
            }

            xSemaphoreGive (robot->_semaphore);

            for (int loop = 0; loop < 3; loop++)
            {
                if (output[loop] != pending[loop])
                {
                    output[loop] = pending[loop];
                    //robot->motor (loop, output[loop]);

                    const unsigned int length = 14;
                    uint8_t message[length];

                    // http://kio4.com/b4a/programas/Appendix%202-LEGO%20MINDSTORMS%20NXT%20Direct%20commands.pdf
                    // http://www.robotappstore.com/Knowledge-Base/-How-to-Control-Lego-NXT-Motors/81.html

                    // mode, byte #6
                    //    2 1 0
                    // 0: 0 0 0 - coast
                    // 1: 0 0 1 - motor on
                    // 2: 0 1 0 - break mode (modifier)
                    // 3: 0 1 1 - motor on | break mode
                    // 4: 1 0 0 - regulated mode (modifier)
                    // 5: 1 0 1 - motor on | regulated mode
                    // 6: 1 1 0 - NA
                    // 7: 1 1 1 - motor on | regulated mode | break mode
                    //
                    message[0] = length - 2;
                    message[1] = 0;

                    message[2] = 0x80; // direct command with no reply
                    message[3] = 0x04; // set output state command
                    message[4] = loop;
                    message[5] = (uint8_t) output[loop];
                    message[6] = 0x07; // motor on
                    message[7] = 0x00; // no regulation 
                    message[8] = 0x00; // no turn ratio
                    message[9] = 0x20; // running
                    message[10] = 0;
                    message[11] = 0;
                    message[12] = 0;
                    message[13] = 0;

                    if (output[loop] == 0)
                    {
                        message[6] = 0; // coast
                        message[9] = 0x00; // disable power
                    }

                    robot->command (message);
                }
            }
        }

        vTaskDelay (10 / portTICK_PERIOD_MS);
    }
}

Robot::Robot (std::string name, LED *led) :
    _connected (false),
    _handle (0),
    _battery (0.0),
    _keepalive (0.0)

{
    _name = name;
    _led = led;
    _output[0] = 0;
    _output[1] = 0;
    _output[2] = 0;

    // create and take the semaphore
    _semaphore = xSemaphoreCreateBinary ();

    // start the LED control task
    _task = NULL;
    xTaskCreate (Robot_task, "Robot_task", 4096, (void *) this, tskIDLE_PRIORITY, &_task);

    // release the semaphore
    xSemaphoreGive (_semaphore);
}

Robot::~Robot ()
{
}

std::string  Robot::name (void)
{
    return (_name);
}

bool Robot::connected (void)
{
    return (_connected);
}

bool Robot::connected (bool state, uint32_t handle)
{
    _connected = state;
    _handle = handle;

    return (connected ());
}

void Robot::battery (void)
{
    const unsigned int length = 2 + 2;
    uint8_t message[length];

    message[0] = length - 2;
    message[1] = 0;

    message[2] = 0x00; // direct command and reply
    message[3] = 0x0b; // get battery level

    // response:
    // [0][1] = lsb and msb of length
    // [2] = command type
    // [3] = command
    // [4] = command status

    command (message);
}

void Robot::keepalive (void)
{
    const unsigned int length = 2 + 2;
    uint8_t message[length];

    message[0] = length - 2;
    message[1] = 0;

    message[2] = 0x00; // direct command with reply
    message[3] = 0x0d; // keep alive

    command (message);
}

void Robot::beep (void)
{
    const unsigned int length = 6 + 2;
    uint8_t message[length];

    message[0] = length - 2;
    message[1] = 0;

    message[2] = 0x80; // direct command with no reply
    message[3] = 0x03; // play a tone
    message[4] = 0x0b; // tone frequency (lsb)
    message[5] = 0x02; // tone frequency (msb)
    message[6] = 0xf4; // tone duration ms (lsb)
    message[7] = 0x01; // tone duration ms (lsb)

    command (message);
}

void Robot::motor (uint8_t port, int8_t speed)
{
    if ((port == 0) || (port == 1) || (port == 2))
    {
        if (xSemaphoreTake (_semaphore, portMAX_DELAY))
        {
            _output[port] = speed;

            xSemaphoreGive (_semaphore);
        }
        else
        {
            return;
        }
    }
}

void Robot::command (uint8_t *data)
{
    // blink the LED
    if (_led)
    {
        _led->on ();
    }

    // determine the internal length of the data
    uint16_t length = (data[1] << 8) + data[0];

    // send the command data
    if (connected ())
    {
        if (xSemaphoreTake (_semaphore, portMAX_DELAY))
        {
            //esp_log_buffer_hex (TAG, data, length + 2);
            esp_spp_write (_handle, length + 2, data);
            xSemaphoreGive (_semaphore);
        }
    }
}

#define UINT16(x) (((x)[1] << 8) | ((x)[0] << 0))
#define UINT32(x) (((x)[3] << 24) | ((x)[2] << 16) | ((x)[1] << 8) | ((x)[0] << 0))

bool Robot::process (uint32_t handle, uint16_t length, uint8_t *data)
{
    // return if data is not from robot handle
    if (handle != _handle)
    {
        return (false);
    }

    // process the available data
    uint16_t remaining = length;

    while (remaining >= 4)
    {
        uint16_t size = 0;

        if (data[3] == 0x0b)
        {
            size = 7;

            if (remaining < size)
            {
                break;
            }

            if (data[4] == 0x00)
            {
                _battery = UINT16 (&data[5]) / 1000.0;
            }
        }
        else if (data[3] == 0x0d)
        {
            size = 9;

            if (remaining < size)
            {
                break;
            }

            if (data[4] == 0x00)
            {
                _keepalive = UINT32 (&data[5]) / 1000.0;
            }
        }
        else
        {
            break;
        }

        remaining = remaining - size;
        data = data + size;
    }

    return (remaining == 0);
}

std::string Robot::status (void)
{
    char buffer[256];

    sprintf (buffer, "{\"connected\": %d}", _connected);

    if (_connected)
    {
        sprintf (buffer, "{\"connected\": %d, \"battery\": %f, \"keepalive\": %f}",
                _connected, _battery, _keepalive);
    }

    return (std::string (buffer));
}
