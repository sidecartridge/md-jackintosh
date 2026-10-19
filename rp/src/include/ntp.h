/**
 * File: ntp.h
 * Author: Diego Parrilla Santamaría
 * Date: October 2026
 * Copyright: 2026 - GOODDATA LABS SL
 * Description: The time of day from an NTP server, without blocking.
 */

#ifndef NTP_H
#define NTP_H

#include <stdbool.h>
#include <stdint.h>

#define NTP_HOST "pool.ntp.org"
#define NTP_PORT 123
#define NTP_DELTA 2208988800u  // seconds from 1900-01-01 to 1970-01-01
#define NTP_MSG_LEN 48         // no authenticator
// How long one lookup or one request may take, and the wait before the next
// attempt after a failure.
#define NTP_ATTEMPT_TIMEOUT_MS 5000
#define NTP_RETRY_MS 5000

/**
 * @brief Run the NTP client: one step, never waiting.
 *
 * Call from the main loop. Nothing happens until the Wi-Fi has an address;
 * then it asks the server, retrying every NTP_RETRY_MS until one answer
 * arrives. After that the time is counted from the RP's timer.
 */
void ntp_pollSync(void);

/**
 * @brief The current UTC time, in seconds since 1970-01-01 00:00.
 *
 * @return false while no server has answered yet; seconds is then untouched.
 */
bool ntp_getUnixTime(uint32_t *seconds);

#endif  // NTP_H
