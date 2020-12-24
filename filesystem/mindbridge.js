class Robot {
    constructor () {
        this.scale = 50;
        this.left = null;
        this.right = null;
        this.connected = false;
        this.token = 0;
        let self = this;

        // 'thread' to send motor control values to the robot
        setInterval (function () {
            // send the control values if connected
            if (self.connected) {
                let data = {};
                data['L'] = this.left;
                data['R'] = this.right;
                data['T'] = this.token;

                $.get ('/drive', data)
                    .done (function (data) {
                    })
                    .fail (function (data) {
                    });
            }
        }, 250);

        // 'thread' to maintain a session with the robot
        setInterval (function () {
            if (!self.connected) {
                $.getJSON ('/open?T=' + self.token, function (data) {
                    try {
                        self.token = data.token;
                        self.connected = (self.token != 0);
                        if (self.connected) {
                            $('.control').addClass ('ready');
                            return;
                        }
                    }
                    catch {
                    }
                    $('.control').removeClass ('ready');
                });
            }
        }, 5 * 1000);

        // begin the session with the motors stopped
        this.stop ();
    }

    stop () {
        this.update (0, 0);
    }

    update (x, y) {
        // determine the motor control values
        let speed = -1 * this.scale * y;
        let left = speed + (this.scale * x / 2);
        let right = speed - (this.scale * x / 2);

        // swap controls if reversing
        if (speed < 0) {
            let temp = left;
            left = right;
            right = temp;
        }

        // discretize to multiples of 10
        left = Math.round (left / 10) * 10;
        right = Math.round (right / 10) * 10;

        // limit the range to -100 .. 100
        this.left = Math.round (Math.min (100, Math.max (-100, left)));
        this.right = Math.round (Math.min (100, Math.max (-100, right)));

        return;
    }
};

class Joystick {
    constructor (element, driver) {
        this.touchpad = element;
        this.joystick = element.find ('#joystick');

        this.driver = driver;

        const self = this;

        this.keys = {};
        this.position = { x: 0, y : 0 };

        this.keyPositions = {
            ArrowUp: { y: 0 },
            ArrowDown: { y: 1},
            ArrowLeft: { x: 0 },
            ArrowRight: { x: 1},
        }

        //
        // handle keyboard events
        //
        $(document)
            .on ('keydown', function (e) {
                self.joystick.addClass ('animated');
                self.keys[e.code] = true;
                self.updateKeys ();
            })
            .on ('keyup', function (e) {
                self.keys[e.code] = false;
                self.updateKeys ();
            });

        window.addEventListener ('resize', () => {
            self.joystick.removeClass ('animated');
            self.update ({x: 0.5, y: 0.5});
        });

        //
        // handle mouse/touch events
        //
        this.touchpad
            .on ('pointerdown pointerenter pointermove', function (e) {
                if (e.buttons === 1) {
                    self.joystick.removeClass ('animated');
                    self.updateMouse (e);
                }
            })
            .on ('pointerup', function (e) {
                self.joystick.addClass ('animated');
                self.update ({x: 0.5, y: 0.5});
            });
    }

    updateKeys () {
        let updatedPosition = { x: 0.5, y: 0.5 };

        Object.entries (this.keys).forEach (e => {
            const key = e[0];
            const value = e[1];

            if (!value) return;

            if (this.keyPositions[key]) {
                updatedPosition.x = this.keyPositions[key].x !== undefined ? this.keyPositions[key].x : updatedPosition.x;
                updatedPosition.y = this.keyPositions[key].y !== undefined ? this.keyPositions[key].y : updatedPosition.y;
            }
        });

        if (this.keys['ArrowLeft'] && this.keys['ArrowRight']) updatedPosition.x = 0.5;
        if (this.keys['ArrowUp'] && this.keys['ArrowDown']) updatedPosition.y = 0.5;

        this.update (updatedPosition);
    }

    updateMouse (e) {
        const touchpadWidth = this.touchpad.width ();
        const touchpadHeight = this.touchpad.height ();

        const joystickWidth = this.joystick.width ();
        const joystickHeight = this.joystick.height ();

        let x = (e.offsetX - (joystickWidth / 2)) / (touchpadWidth - joystickWidth);
        let y = (e.offsetY - (joystickHeight / 2)) / (touchpadHeight - joystickHeight);

        // Clamp x/y from 0-1
        x = Math.max (0, Math.min (x, 1));
        y = Math.max (0, Math.min (y, 1));

        const snappedPosition = this.snapHotspots ({x, y});
        this.update (snappedPosition);
    }

    distSq (x1, y1, x2, y2) {
        return ((x2-x1) * (x2-x1)) + ((y2-y1) * (y2-y1));
    }

    checkSpot (pos, x2, y2) {
        if (this.distSq (pos.x, pos.y, x2, y2) < 0.03) return { x: x2, y: y2 };
        return pos;
    }

    snapHotspots (position) {
        let updatedPosition = { ...position };

        const hotspots = [
            [0.0, 0.0], [0.5, 0.0], [1.0, 0.0],
            [0.0, 0.5],           , [1.0, 0.5],
            [0.0, 1.0], [0.5, 1.0], [1.0, 1.0]
        ];

        hotspots.forEach (h => updatedPosition = this.checkSpot (updatedPosition, h[0], h[1]));

        return updatedPosition;
    }

    update (updatedPosition) {
        const touchpadWidth = this.touchpad.width ();
        const touchpadHeight = this.touchpad.height ();

        const joystickWidth = this.joystick.width ();
        const joystickHeight = this.joystick.height ();

        // Double the set margin of 3%
        const totalMargin = '6%';

        this.joystick.css ('left', `calc(${updatedPosition.x} * (${touchpadWidth - joystickWidth}px - ${totalMargin}))`);
        this.joystick.css ('top', `calc(${updatedPosition.y} * (${touchpadHeight - joystickHeight}px - ${totalMargin}))`);

        // the lazy way to check differences for arbitray data
        if (JSON.stringify (updatedPosition) !== JSON.stringify (this.position)) {
            this.position = updatedPosition;
            this.driver.update ((updatedPosition.x * 2) - 1, (updatedPosition.y * 2) - 1);
        }
    }
};
