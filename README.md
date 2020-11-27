# mindbridge

This code is run on an ESP32 with Bluetooth, WiFi and a GPIO-interfaced camera
to enable remote control of a LEGO Mindstorms robot.

The code is written for and built using the Espresif development framework and
tools (https://docs.espressif.com/projects/esp-idf/en/latest/esp32/index.html)
and not the higher-level Arduino environment.

## Configuration

The project needs to be configured before building such that the code knows
the local WiFi credentials and the Bluetooth SSP name of the robot ot control.

WiFi Configuration
* ssid
* password
* retries

Bluetooth Configuration
* name

## Operation

The code runs on the ESP32 to provide a web interface with streaming video
and a joystick/pad to control the robot's drive motors.

# Remaining Work

- [ ] rework the project configuration
- [ ] rework the WiFi initialization
- [ ] clean up the camera code
- [ ] rework the camera acquisitions into a shared/async task
- [ ] remove duplicate camera code
- [ ] integrate the Bluetooth code
- [ ] integrate the robot interface code
- [ ] add activity LED
- [ ] limit client access to ensure responsiveness
- [ ] change streaming video to use pre-acquired frames from the task

