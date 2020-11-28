class Robot {
    constructor () {
        this.connected = false;
        this.left = null;
        this.right = null;
        this.token = 0;

        this.stop ();
        let self = this;

        (function ping () {
            $.getJSON ('/open?T=' + self.token, function (data) {
                try {
                    self.token = data.token;
                    self.connected = (self.token != 0);
                    if (self.connected) {
                        $('.control').addClass ('ready');
                        //$('#overlay').addClass ('hidden');
                    }
                    else {
                        $('.control').removeClass ('ready');
                        //$('#overlay').removeClass ('hidden');
                    }
                }
                catch {
                }
            });

            setTimeout (ping, 1 * 1000);
        })();

    }

    stop () {
        this.update (0, 0);
    }

    update (x, y) {
        // determine the motor control values
        let speed = y;
        let left = speed + (x / 2);
        let right = speed - (x / 2);

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
        left = Math.round (Math.min (100, Math.max (-100, left)));
        right = Math.round (Math.min (100, Math.max (-100, right)));

        // avoid sending duplicate values
        let data = {};

        if (this.left != left) {
            data['L'] = left;
            this.left = left;
        }

        if (this.right != right) {
            data['R'] = right;
            this.right = right;
        }

        if (Object.keys (data).length == 0) {
            return;
        }

        data['T'] = this.token;

        // return if not connected
        if (!this.connected) {
            return;
        }

        // send the control values
        $.get ('/drive', data)
            .done (function (data) {
                console.log (data);
            })
            .fail (function (data) {
                console.log (data);
            });
    }
};

class Joystick {
    constructor (element, driver, scale = 100) {
        this.element = $(element);
        this.element.addClass ('animated');

        this.driver = driver;
        this.scale = scale;
        this.active = false;

        this.center = {
            'x': parseFloat (this.element.css ('left'), 10),
            'y': parseFloat (this.element.css ('top'), 10)
        };

        this.start = {
            'x': parseFloat (this.element.css ('left'), 10),
            'y': parseFloat (this.element.css ('top'), 10)
        };

        this.mapping = {
            'ArrowUp': {'x': 0, 'y': 0.5},
            'ArrowDown': {'x': 0, 'y': -0.5},
            'ArrowLeft': {'x': -0.5, 'y': 0},
            'ArrowRight': {'x': 0.5, 'y': 0}
        };

        let self = this;

        //
        // handle keyboard events
        //
        $(document)
            .on ('keydown', function (e) {
                if (e.code in self.mapping) {
                    self.active = false;

                    e.preventDefault ();
                    e.stopPropagation ();

                    if (!e.originalEvent.repeat) {
                        let off_x = 0.75 * self.element.parent ().parent ().width ();
                        self.element.css ('left', self.center.x + (self.mapping[e.code].x * off_x) + 'px');
                        let off_y = 0.75 * self.element.parent ().parent ().height ();
                        self.element.css ('top', self.center.y + (-self.mapping[e.code].y * off_y) + 'px');

                        let x = self.mapping[e.code].x * self.scale;
                        let y = self.mapping[e.code].y * self.scale;

                        self.driver.update (x, y);
                    }
                }
            })
            .on ('keyup', function (e) {
                if (e.code in self.mapping) {
                    self.end (e);
                }
            });

        //
        // handle directional pad mouse events
        //
        $('.pad')
            .on ('mousedown touchstart', function (e) {
                e.preventDefault ();
                e.stopPropagation ();

                let pad = $(this);

                let off_x = 0.75 * self.element.parent ().parent ().width ();
                self.element.css ('left', self.center.x + (pad.data ('x') * off_x) + 'px');
                let off_y = 0.75 * self.element.parent ().parent ().height ();
                self.element.css ('top', self.center.y + (-pad.data ('y') * off_y) + 'px');

                let x = pad.data ('x') * self.scale;
                let y = pad.data ('y') * self.scale;

                self.driver.update (x, y);
            });

        //
        // handle mouse/touch tracking events
        //
        this.element
            .on ('mousedown', function (e) {
                self.begin (e, e.clientX, e.clientY);
            })
            .on ('touchstart', function (e) {
                self.begin (e, e.originalEvent.touches[0].clientX, e.originalEvent.touches[0].clientY);
            })
            .on ('mousemove', function (e) {
                self.update (e, e.clientX, e.clientY);
            })
            .on ('touchmove', function (e) {
                self.update (e, e.originalEvent.touches[0].clientX, e.originalEvent.touches[0].clientY);
            });

        //
        // end if the mouse exits the trackpad area
        //
        this.element.parent ().parent ()
            .on ('mouseup mouseleave touchend', function (e) {
                self.end (e);
            });
    }

    begin (e, x, y) {
        e.preventDefault ();
        e.stopPropagation ();

        this.active = true;
        this.element.removeClass ('animated');
        this.start = {'x': x, 'y': y};
    }

    update (e, x, y) {
        if (!this.active) {
            return;
        }

        e.preventDefault ();
        e.stopPropagation ();

        let limit = (this.element.parent ().parent ().width () / 2) - (this.element.width () / 2) - 12; // HACK
        let scale = this.scale / limit;

        let dx = this.start.x - x;
        if ((dx >= -limit) && (dx <= limit)) {
            this.element.css ('left', this.center.x + (-dx) + "px");
        }

        let dy = -1 * (this.start.y - y);
        if ((dy >= -limit) && (dy <= limit)) {
            this.element.css ('top', this.center.y + (dy) + "px");
        }

        dx = Math.min (this.scale, Math.max (-this.scale, -dx * scale));
        dy = Math.min (this.scale, Math.max (-this.scale, -dy * scale));

        this.driver.update (dx, dy);
    }

    end (e) {
        try {
            if (e.cancelable) {
                e.preventDefault ();
                e.stopPropagation ();
            }
        }
        catch {
        }

        this.active = false;

        this.element.addClass ('animated');
        this.element.css ('left', this.center.x + 'px');
        this.element.css ('top', this.center.y + 'px');

        this.driver.stop ();
    }
};
