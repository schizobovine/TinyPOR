//
// tinyPOR.ino - ATTiny85 Power-On/Reset controller for the Raspberry Pi
//
// Author: Sean Caulfield <sean@yak.net>
// License: GPLv2.0
//
//

#include <Arduino.h>
#include <Bounce2.h>


// ----------------------------------------------------------------------------
// Terrible ASCII Art Wiring Diagram
// ----------------------------------------------------------------------------
//
//              +--------+
//          RST | 1    8 | VCC  [ 3.3V ]
// [ BUTT ]  D3 | 2    7 | D2   [ SHUTDOWN (BCM23) ]
// [ !LED ]  D4 | 3    6 | D1   [ ACKOFF   (BCM24) ]
//          GND | 4    5 | D0   [ RUN (RPi reset)  ] 
//              +--------+
//
// ----------------------------------------------------------------------------
// Pin Mappings
// ----------------------------------------------------------------------------
//
// BUTT     - Connected to physical momentary pushbutton (accidentally wired as
//            active HIGH because the LED was wired in backwards).
//
// !LED     - LED inside the button above. Since I wired this to the cathode
//            instead of the anode, I had to run +3v3 to the button assembly
//            and just twiddle this active LOW to get the LED to light up
//            (signifying powering-off and safe-to-power-off)
//
// SHUTDOWN - GPIO configured as input device via gpio-keyboard driver which
//            triggerhappyd picks up and executes a shutdown script.
//
// ACKOFF   - GPIO configured as goes-high-on-poweroff, allowing us to detect
//            when the Pi has *completely* shutdown everything. NOTE: this is a
//            "soft" shutdown; see "Soft Shutdown" section below for details.
//
// RUN      - Reset line for the PI. Held HIGH normally, we'll need to pull it
//            LOW for a tick to get the Pi to go from the "halted" state to
//            trying to bootload itself again. Needed for long-push-to-reset
//            and general reset functionality (if the user doesn't power off
//            the RPI+ATTiny assembly immediately after).
//
//
// ----------------------------------------------------------------------------
// "Soft Shutdown"
// ----------------------------------------------------------------------------
//
// When you call /sbin/halt on the RPi, it eventually will stop all processes
// running on the CPU and put itself in a "halt" state, that theoretically
// shouldn't consume much power (but does). Why? Because a lot of other crap on
// the board is still nominally on -- power is still going to the regulator and
// anything else powered from the 5V input lines.
//
// To fully switch stuff off, we'd need a MOSFET or relay or something to
// switch that circuit. This program is NOT intended for that situation: this
// is so I can confirm visually an embedded device is safe to power off and
// pull the cable.
//
// ----------------------------------------------------------------------------
// State Diagram
// ----------------------------------------------------------------------------
//
//                 +-------------------+
//                 V                   |
//           +-----+------+            |
//           |+----+-----+|            |
// START =>  || STATE_ON ||            |
//           |+----+-----+|            |
//           +-----+------+            |
//                 |                   |
//                 V                   |
//            +----+----+              |
//           +           +             |
//          +     BUTT    +            |
//         +   pushed for  +           |
//         +   >=500ms &   +           |
//          +   released  +            |
//           +           +             |
//            +----+----+              |
//                 |                   |
//                 V                   |
//     +-----------+-----------+       |
//     |  STATE_SHUTTING_DOWN  |       |
//     +-----------+-----------+       |
//                 |                   |
//                 V                   |
//            +----+----+              |
//           +           +             |
//          +   ACKOFF    +            |
//          +  goes HIGH  +            |
//           +           +             |
//            +----+----+              |
//                 |                   |
//                 V                   |
//           +-----+-----+             |
//           | STATE_OFF |             |
//           +-----+-----+             |
//                 |                   |
//                 V                   |
//            +----+----+              |
//           +           +             |
//          +    BUTT     +            |
//         +   pushed for  +           |
//         +  >=500ms then +           |
//          +  released   +            |
//           +           +             |
//            +----+----+              |
//                 |                   |
//                 V                   |
//         +-------+--------+          |
//         | STATE_STARTING |          |
//         +-------+--------+          |
//                 |                   |
//                 V                   |
//           +-----+-----+             |
//          +             +            |
//         +   wait 500ms  +-----------+
//          +             +
//           +-----+-----+
// 

#define PIN_RUN       0
#define PIN_SHUTDOWN  2
#define PIN_ACKOFF    1
#define PIN_BUTT      3
#define PIN_BUTT_LED  4

// Number of milliseconds to wait for button signal to stabilize
#define DEBOUNCE_MS 100

// How long to hold the reset pulse for the RPi to do a (re)start
#define RESET_HOLD_MS 500

// How long to hold the button for to trigger state change (ms)
#define BUTTON_PRESS_MS 500

//
// States for state machine (transitions are one-way from each to the next,
// wrapping around at the end).
//

typedef enum {
  STATE_OFF,
  STATE_STARTING,
  STATE_ON,
  STATE_SHUTTING_DOWN
} state_t;

////////////////////////////////////////////////////////////////////////////////
// GLOBALS
////////////////////////////////////////////////////////////////////////////////

Bounce butt;               // User-pushed button
unsigned long press_start; // For detecting long-press
unsigned long reset_start; // For holding reset signal low
unsigned long now;         // Current time in millis
state_t curr;              // Current state

////////////////////////////////////////////////////////////////////////////////
// SETUP
////////////////////////////////////////////////////////////////////////////////

void setup() {

  // For detecting when shutdown is complete to transition to STATE_OFF. Active
  // HIGH when RPi has completed shutdown.
  digitalWrite(PIN_ACKOFF, LOW);
  pinMode(PIN_ACKOFF, INPUT);

  // For some reason I could not get this working as active low, so using
  // active high because screw it (also the RPi shouldn't sink too much current
  // from us, and it's 3v3 anyway.
  digitalWrite(PIN_SHUTDOWN, LOW);
  pinMode(PIN_SHUTDOWN, OUTPUT);

  // For these , though, I have a reason why they have to be weird: I
  // accidentally wired the LED backwards, so have to send +3v3 to it instead
  // of 0V as the third wire. So explicitly write LOW here and then set INPUT
  // mode so hopefully it'll detect the pull-high without frying anything.
  digitalWrite(PIN_BUTT, LOW);
  pinMode(PIN_BUTT, INPUT);

  // ...and HIGH here to go LOW when we need to blink it (blink it good)
  pinMode(PIN_BUTT_LED, OUTPUT);
  digitalWrite(PIN_BUTT_LED, HIGH);

  // Tri-state / floating. The RPi board should be pulling this up (and if we
  // did, anyone else trying to reset it would be overriden).
  digitalWrite(PIN_RUN, HIGH);
  pinMode(PIN_RUN, INPUT);

  // Initialze state as "ON" because otherwise we'll start off thinking we're
  // off but the RPI is probably just booting up (since the power just came
  // on).
  curr = STATE_ON;
  press_start = 0;
  reset_start = 0;

  // Setup button debouncer
  butt.attach(PIN_BUTT);
  butt.interval(DEBOUNCE_MS);

}

////////////////////////////////////////////////////////////////////////////////
// MAIN
////////////////////////////////////////////////////////////////////////////////

void loop() {

  // Check for button/shutdown acknowledge state changes
  butt.update();

  // Store button initial press time
  if (butt.rose()) {
    press_start = millis();
  }

  switch (curr) {

    // Power off, button push, initiate startup by dragging the RPi's
    // RUN line low for a few ticks. Turn LED on while pulse is active.
    case STATE_OFF:

      if (butt.fell()) {
        now = millis();
        if ((now - press_start) >= BUTTON_PRESS_MS) {
          pinMode(PIN_RUN, OUTPUT);
          digitalWrite(PIN_RUN, LOW); // RUN := LOW (initiate reset)
          digitalWrite(PIN_BUTT_LED, LOW); // LED ON
          reset_start = now;
          curr = STATE_STARTING;
        }
        press_start = 0;
      }

      break;

    // Power was off and we've started the reset pulse; check if it's time to
    // stop and transition to the "on" state. Note that I probably could use
    // some GPIO as an "ACK, I am now awake", but just want to test this for
    // now.
    case STATE_STARTING:
      
      now = millis();
      if (now - reset_start >= RESET_HOLD_MS) {
        digitalWrite(PIN_RUN, HIGH);
        pinMode(PIN_RUN, INPUT); // RUN := high-Z
        digitalWrite(PIN_BUTT_LED, HIGH); // LED OFF
        curr = STATE_ON;
      }
      break;

    // Power on, button pushed, initiate shutdown after press duration
    case STATE_ON:

      if (butt.fell()) {
        now = millis();
        if ((now - press_start) >= BUTTON_PRESS_MS) {
          digitalWrite(PIN_SHUTDOWN, HIGH);   // "Press" shutdown button
          delay(100);                         // THIS IS TERRIBLE
          digitalWrite(PIN_SHUTDOWN, LOW);    // "Release" shutdown button
          //digitalWrite(PIN_BUTT_LED, HIGH);   // LED OFF
          curr = STATE_SHUTTING_DOWN;
        }
        press_start = 0;
      }

      break;

    // Currently shutting down, button does NOTHING (say it in the
    // same voice as you would for goggles) until we've confirmed the
    // rpi is actually down.
    case STATE_SHUTTING_DOWN:
      if (digitalRead(PIN_ACKOFF) == HIGH) {
        digitalWrite(PIN_BUTT_LED, LOW); // LED ON
        curr = STATE_OFF;
      }
      break;

    // TODO Maybe make this have a long-press behavior like ATX power
    // supply buttons?

    default: break;
  }

}
