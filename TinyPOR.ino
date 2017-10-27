/*
 * tinyPOR.ino - ATTiny85 Power-On/Reset controller for the Raspberry Pi
 *
 * Author: Sean Caulfield <sean@yak.net>
 * License: GPLv2.0
 *
 */

#include <Arduino.h>
#include <Bounce2.h>


//
// Terrible ASCII Art Wiring Diagram
// ----------------------------------------------------------------------------
//
//  +-w-+ Resistor (10K
// +-||-+ Capacitor (100nF to 1uF)
//
//                 VCC--+     +--GND--+
//                       \    |       |
//                       v    v       |
//                   +-w-+-||-+       v
//                   \ +--------+     +-+=> GND
//   LED <=+---+ + RST | 1    8 | VCC +-+=> 3v3
//   GND <=+-+ \-+  D3 | 2    7 | D2  +-+=> RUN [SCLK] (RPi reset)
//  BUTT <=+-\---+  D4 | 3    6 | D1  +-+=> OFF [MISO] (GPIO keyboard)
//            \--+ GND | 4    5 | D0  +-+=> ACK [MOSI] (GPIO high on shutdown)
//                     +--------+
//

#define PIN_OFF       0 // MOSI
#define PIN_ACK       1 // MISO
#define PIN_RUN       2 // SCLK
#define PIN_BUTT      3
#define PIN_BUTT_LED  4

// Number of milliseconds lockout after a signal is triggered
#define DEBOUNCE_MS 10

// How long to hold the reset pulse for the RPi to do a (re)start
#define RESET_HOLD_MS 500

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
Bounce ack;                // Shutdown complete signal from rpi (active high)
unsigned long press_start; // For detecting long-press
unsigned long reset_start; // For holding reset signal low
state_t curr;              // Current state

////////////////////////////////////////////////////////////////////////////////
// SETUP
////////////////////////////////////////////////////////////////////////////////

void setup() {

  pinMode(PIN_BUTT_LED, OUTPUT);
  pinMode(PIN_BUTT, INPUT_PULLUP);
  pinMode(PIN_ACK, INPUT);

  // For some reason I could not get this working as active low, so using
  // active high because screw it (also the RPi shouldn't sink too much current
  // from us, and it's 3v3 anyway.
  digitalWrite(PIN_OFF, LOW);
  pinMode(PIN_OFF, OUTPUT);

  // Tri-state but leave as HIGH so it won't trigger a reset when it flips to
  // output.
  digitalWrite(PIN_RUN, HIGH);
  pinMode(PIN_RUN, INPUT);

  butt.attach(PIN_BUTT, DEBOUNCE_MS);
  ack.attach(PIN_ACK, DEBOUNCE_MS);

  curr = STATE_OFF;
  press_start = 0;
  reset_start = 0;

}

////////////////////////////////////////////////////////////////////////////////
// MAIN
////////////////////////////////////////////////////////////////////////////////

void loop() {

  // Check for button/shutdown acknowledge state changes
  butt.update();
  ack.update();

  switch (curr) {

    // Power off, button push, initiate startup by dragging the RPi's
    // RUN line low for a few ticks. Turn LED on while pulse is active.
    case STATE_OFF:
      if (butt.fell()) {
        digitalWrite(PIN_RUN, LOW);
        pinMode(PIN_RUN, OUTPUT);
        digitalWrite(PIN_BUTT_LED, HIGH);
        reset_start = millis();
        curr = STATE_STARTING;
      }
      break;

    // Power was off and we've started the reset pulse; check if it's time to
    // stop and transition to the "on" state. Note that I probably could use
    // some GPIO as an "ACK, I am now awake", but just want to test this for
    // now.
    case STATE_STARTING:
      if (millis() - reset_start >= RESET_HOLD_MS) {
        pinMode(PIN_RUN, INPUT);
        digitalWrite(PIN_RUN, HIGH);
        digitalWrite(PIN_BUTT_LED, LOW);
        curr = STATE_ON;
      }
      break;

    // Power on, button pushed, initiate shutdown
    case STATE_ON:
      if (butt.fell()) {
        digitalWrite(PIN_BUTT_LED, HIGH);
        pinMode(PIN_OFF, OUTPUT);
        digitalWrite(PIN_OFF, LOW);
        curr = STATE_SHUTTING_DOWN;
      }
      break;

    // Currently shutting down, button does NOTHING (say it in the
    // same voice as you would for goggles) until we've confirmed the
    // rpi is actually down.
    case STATE_SHUTTING_DOWN:
      if (ack.rose()) {
        pinMode(PIN_OFF, INPUT);
        digitalWrite(PIN_OFF, LOW);
        digitalWrite(PIN_BUTT_LED, LOW);
        curr = STATE_OFF;
      }
      break;

    // TODO Maybe make this have a long-press behavior like ATX power
    // supply buttons?

    default: break;
  }

}
