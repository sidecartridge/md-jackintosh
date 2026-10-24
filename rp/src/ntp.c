/**
 * File: ntp.c
 * Author: Diego Parrilla Santamaría
 * Date: October 2026
 * Copyright: 2026 - GOODDATA LABS SL
 * Description: The time of day from an NTP server, without blocking. The
 *              request and the answer's checks come from md-drives-emulator's
 *              rtc.c, whose query waits up to 5 s; this one is a state
 *              machine stepped from the main loop, so the command ring keeps
 *              draining.
 */

#include "ntp.h"

#include <stdio.h>
#include <string.h>

#include "debug.h"
#include "lwip/dns.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"
#include "network.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

typedef enum {
  NTP_STATE_IDLE,       // waiting for the network or the next attempt
  NTP_STATE_RESOLVING,  // the server's name is being looked up
  NTP_STATE_WAITING,    // the request is out
  NTP_STATE_SYNCED,
} NtpState;

static NtpState state = NTP_STATE_IDLE;
static struct udp_pcb *pcb = NULL;
static ip_addr_t serverIp;
static bool resolved = false;
static absolute_time_t deadline;
static absolute_time_t nextAttempt;
static bool nextAttemptSet = false;
static uint32_t syncedUnix = 0;
static uint64_t syncedAtUs = 0;
static char serverHost[NTP_HOST_SIZE] = NTP_HOST;
static uint16_t serverPort = NTP_PORT;

static void ntp_retryLater(void) {
  state = NTP_STATE_IDLE;
  nextAttempt = make_timeout_time_ms(NTP_RETRY_MS);
  nextAttemptSet = true;
}

static void ntp_dnsFoundCb(const char *name, const ip_addr_t *ipaddr,
                           void *arg) {
  (void)name;
  (void)arg;
  // An answer to an attempt that already timed out changes nothing: the next
  // attempt finds the name in lwIP's cache.
  if (state != NTP_STATE_RESOLVING) return;
  if (ipaddr == NULL) {
    DPRINTF("ntp: %s not found\n", serverHost);
    ntp_retryLater();
    return;
  }
  serverIp = *ipaddr;
  resolved = true;
}

static void ntp_recvCb(void *arg, struct udp_pcb *upcb, struct pbuf *p,
                       const ip_addr_t *addr, u16_t port) {
  (void)arg;
  (void)upcb;
  if (p == NULL) return;
  if (state != NTP_STATE_WAITING || p->tot_len != NTP_MSG_LEN ||
      !ip_addr_cmp(&serverIp, addr) || port != serverPort) {
    pbuf_free(p);
    return;
  }
  uint8_t mode = pbuf_get_at(p, 0) & 0x07;  // 4: a server's answer
  uint8_t stratum = pbuf_get_at(p, 1);      // 0: a kiss-o'-death
  uint32_t transmit = 0;                    // the transmit timestamp's seconds
  pbuf_copy_partial(p, &transmit, sizeof(transmit), 40);
  pbuf_free(p);
  transmit = lwip_ntohl(transmit);
  if (mode != 4 || stratum == 0 || transmit < NTP_DELTA) {
    DPRINTF("ntp: answer refused (mode %u, stratum %u)\n", (unsigned)mode,
            (unsigned)stratum);
    ntp_retryLater();
    return;
  }
  syncedUnix = transmit - NTP_DELTA;
  syncedAtUs = time_us_64();
  state = NTP_STATE_SYNCED;
  udp_remove(pcb);
  pcb = NULL;
  DPRINTF("ntp: synced, %lu s since 1970 UTC\n", (unsigned long)syncedUnix);
}

static void ntp_sendRequest(void) {
  if (pcb == NULL) {
    pcb = udp_new_ip_type(IPADDR_TYPE_ANY);
    if (pcb == NULL) {
      DPRINTF("ntp: no UDP control block\n");
      ntp_retryLater();
      return;
    }
    udp_recv(pcb, ntp_recvCb, NULL);
  }
  struct pbuf *pb = pbuf_alloc(PBUF_TRANSPORT, NTP_MSG_LEN, PBUF_RAM);
  if (pb == NULL) {
    DPRINTF("ntp: no pbuf for the request\n");
    ntp_retryLater();
    return;
  }
  uint8_t *req = (uint8_t *)pb->payload;
  memset(req, 0, NTP_MSG_LEN);
  req[0] = 0x1b;  // version 3, client
  err_t err = udp_sendto(pcb, pb, &serverIp, serverPort);
  pbuf_free(pb);
  if (err != ERR_OK) {
    DPRINTF("ntp: request not sent: %s\n", lwip_strerr(err));
    ntp_retryLater();
    return;
  }
  state = NTP_STATE_WAITING;
  deadline = make_timeout_time_ms(NTP_ATTEMPT_TIMEOUT_MS);
}

void ntp_setServer(const char *host, uint16_t port) {
  snprintf(serverHost, sizeof(serverHost), "%s", host);
  serverPort = port;
  DPRINTF("ntp: server %s, port %u\n", serverHost, (unsigned)serverPort);
}

void ntp_pollSync(void) {
  if (state == NTP_STATE_SYNCED) return;
  ip_addr_t ip = network_getCurrentIp();
  if (ip_addr_isany(&ip)) {
    // No network, or it dropped: a request in flight is abandoned.
    state = NTP_STATE_IDLE;
    return;
  }
  absolute_time_t now = get_absolute_time();

  cyw43_arch_lwip_begin();
  switch (state) {
    case NTP_STATE_IDLE: {
      if (nextAttemptSet && absolute_time_diff_us(now, nextAttempt) > 0) break;
      resolved = false;
      state = NTP_STATE_RESOLVING;
      deadline = make_timeout_time_ms(NTP_ATTEMPT_TIMEOUT_MS);
      err_t err =
          dns_gethostbyname(serverHost, &serverIp, ntp_dnsFoundCb, NULL);
      if (err == ERR_OK) {
        ntp_sendRequest();  // cached, or a literal address
      } else if (err != ERR_INPROGRESS) {
        DPRINTF("ntp: lookup not started (%d)\n", (int)err);
        ntp_retryLater();
      }
      break;
    }
    case NTP_STATE_RESOLVING:
      if (resolved) {
        ntp_sendRequest();
      } else if (absolute_time_diff_us(now, deadline) <= 0) {
        DPRINTF("ntp: lookup timed out\n");
        ntp_retryLater();
      }
      break;
    case NTP_STATE_WAITING:
      if (absolute_time_diff_us(now, deadline) <= 0) {
        DPRINTF("ntp: no answer\n");
        ntp_retryLater();
      }
      break;
    default:
      break;
  }
  cyw43_arch_lwip_end();
}

bool ntp_getUnixTime(uint32_t *seconds) {
  if (state != NTP_STATE_SYNCED) return false;
  *seconds = syncedUnix + (uint32_t)((time_us_64() - syncedAtUs) / 1000000u);
  return true;
}
