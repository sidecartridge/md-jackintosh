/**
 * File: blink.h
 * Author: Diego Parrilla Santamaría
 * Date: November 2024, February 2026
 * Copyright: 2024-2026 - GOODDATA LABS SL
 * Description: Header file for the blinking functions
 */

#ifndef BLINK_H
#define BLINK_H

#include "constants.h"
#include "debug.h"
#include "hardware/clocks.h"
#include "pico/stdlib.h"

#ifdef CYW43_WL_GPIO_LED_PIN
#include "network.h"
#include "pico/cyw43_arch.h"
#endif

#define CHARACTER_GAP_MS 700
// How long the LED stays on after the last activity pulse.
#define BLINK_ACTIVITY_ON_US 25000
// A counted sequence's flashes.
#define BLINK_SEQUENCE_ON_MS 200
#define BLINK_SEQUENCE_OFF_MS 200

/**
 * @brief   Blinks an LED to represent a given character in Morse code.
 *
 * @param   chr  The character to blink in Morse code.
 *
 * @details This function searches for the provided character in the
 *          `morseAlphabet` structure array to get its Morse code
 * representation. If found, it then blinks an LED in the pattern of dots and
 * dashes corresponding to the Morse code of the character. The LED blinks are
 *          separated by time intervals defined by constants such as
 * DOT_DURATION_MS, DASH_DURATION_MS, SYMBOL_GAP_MS, and CHARACTER_GAP_MS.
 *
 * @return  void
 */
/**
 * @brief Turns off the LED.
 *
 * This function turns off the LED by setting the appropriate GPIO pin to low.
 * It checks if the CYW43_WL_GPIO_LED_PIN is defined and uses it if available.
 * Otherwise, it defaults to using the PICO_DEFAULT_LED_PIN.
 */
void blink_off();

/**
 * @brief Turns on the LED.
 *
 * This function turns on the LED by setting the appropriate GPIO pin high.
 * It checks if the `CYW43_WL_GPIO_LED_PIN` is defined and uses it to set the
 * GPIO pin. If not defined, it defaults to using `PICO_DEFAULT_LED_PIN`.
 */
void blink_on();

/**
 * @brief Toggles the blink state and updates the blink status accordingly.
 *
 * This function toggles the global variable `blink_state` between true and
 * false. If `blink_state` is true, it calls the `blink_on()` function to turn
 * on the blink. If `blink_state` is false, it calls the `blink_off()` function
 * to turn off the blink.
 */
void blink_toogle();

/**
 * @brief Emits a short non-blocking activity pulse on the LED.
 *
 * The LED is turned on at once (written only if it is off) and turned off by
 * blink_poll() BLINK_ACTIVITY_ON_US after the last pulse. On a Pico W the LED
 * is on the Wi-Fi chip and every write is a bus transaction, so call it after
 * the ST's answer, never before.
 */
void blink_activityPulse(void);

/**
 * @brief Starts a counted LED flash sequence.
 *
 * The LED flashes `count` times without blocking the caller. Activity pulses
 * are ignored until it ends.
 *
 * @param count Number of flashes to emit.
 */
void blink_startCountSequence(uint8_t count);

/**
 * @brief Advances a counted sequence, and turns the activity LED off once its
 * pulse has run out.
 *
 * Call this regularly from the main loop.
 */
void blink_poll(void);

#endif  // BLINK_H
