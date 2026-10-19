#include "blink.h"

static bool blinkState = false;
static absolute_time_t blinkTime = {0};
static bool blinkActivityLedOn = false;
static absolute_time_t blinkActivityOffTime = {0};
static bool blinkSequenceActive = false;
static bool blinkSequenceLedOn = false;
static uint8_t blinkSequenceRemaining = 0;
static absolute_time_t blinkSequenceNext = {0};
void blink_on() {
#if defined(CYW43_WL_GPIO_LED_PIN)
  network_initChipOnly();
  cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
#else
  gpio_put(PICO_DEFAULT_LED_PIN, 1);
#endif
}

void blink_off() {
#if defined(CYW43_WL_GPIO_LED_PIN)
  network_initChipOnly();
  cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
#else
  gpio_put(PICO_DEFAULT_LED_PIN, 0);
#endif
}

void blink_toogle() {
  // Blink when timeout
  if ((absolute_time_diff_us(get_absolute_time(), blinkTime) < 0)) {
    blinkState = !blinkState;
    blinkTime = make_timeout_time_ms(CHARACTER_GAP_MS);
    if (blinkState) {
      blink_on();
    } else {
      blink_off();
    }
  }
}

void blink_activityPulse(void) {
  if (blinkSequenceActive) return;
  // During a burst the LED is already on: only the first pulse writes it, the
  // rest push the off time back.
  if (!blinkActivityLedOn) {
    blinkActivityLedOn = true;
    blink_on();
  }
  blinkActivityOffTime = make_timeout_time_us(BLINK_ACTIVITY_ON_US);
}

void blink_startCountSequence(uint8_t count) {
  if (count == 0) return;
  blinkSequenceActive = true;
  blinkSequenceLedOn = true;
  blinkSequenceRemaining = count;
  blink_on();
  blinkSequenceNext = make_timeout_time_ms(BLINK_SEQUENCE_ON_MS);
}

void blink_poll(void) {
  if (blinkSequenceActive) {
    if (absolute_time_diff_us(get_absolute_time(), blinkSequenceNext) >= 0) {
      return;
    }
    if (blinkSequenceLedOn) {
      blink_off();
      blinkSequenceLedOn = false;
      if (--blinkSequenceRemaining == 0) {
        blinkSequenceActive = false;
        return;
      }
      blinkSequenceNext = make_timeout_time_ms(BLINK_SEQUENCE_OFF_MS);
      return;
    }
    blink_on();
    blinkSequenceLedOn = true;
    blinkSequenceNext = make_timeout_time_ms(BLINK_SEQUENCE_ON_MS);
    return;
  }
  if (!blinkActivityLedOn) return;
  if (absolute_time_diff_us(get_absolute_time(), blinkActivityOffTime) > 0) {
    return;
  }
  blink_off();
  blinkActivityLedOn = false;
}
