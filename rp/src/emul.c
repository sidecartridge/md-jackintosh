/**
 * File: emul.c
 * Author: Diego Parrilla Santamaría
 * Date: February 2025, February 2026
 * Copyright: 2025-2026 - GOODDATA LABS
 * Description: Template code for the core emulation
 */

#include "emul.h"

#include <stdint.h>

// inclusw in the C file to avoid multiple definitions
#include "aconfig.h"
#include "chandler.h"
#include "commemul.h"
#include "constants.h"
#include "debug.h"
#include "devdisktest.h"  // Debug-only disk self-test, from the card or SWD
#include "devhooks.h"     // Debug-only SWD mailbox; include in this file only
#include "ff.h"
#include "gconfig.h"
#include "macdisk.h"
#include "memfunc.h"
#include "network.h"
#include "ntp.h"
#include "pico/stdlib.h"
#include "reset.h"
#include "romemul.h"
#include "sdcard.h"
#include "select.h"
#include "target_firmware.h"  // Include the target firmware binary

#define SLEEP_LOOP_MS 100

// How long a sentinel command the ST must act on before this side moves on
// is held. The boot screen reads the sentinel in a tight loop once it has
// asked for Booster; the hold is well over that.
#define SENTINEL_HOLD_MS 500

// The sentinel value that resets the ST: CMD_RESET in
// target/atarist/src/main.s.
#define EMUL_SENTINEL_RESET 0x1

enum {
  APP_MODE_SETUP = 255  // Setup
};

// Keep active loop or exit
static bool keepActive = true;

// Polling tick used as the network poll callback so command handling stays
// alive during multi-second WiFi operations.
#if defined(_DEBUG) && (_DEBUG != 0)
// Heap held on request by `swd.py app heap_hold`, to test running out of it.
typedef struct DevhooksHeldBlock {
  struct DevhooksHeldBlock *next;
} DevhooksHeldBlock;
static DevhooksHeldBlock *devhooksHeldHeap = NULL;

// App commands sent over SWD by tools/dev/swd.py (see emul.h).
static uint32_t emul_devhooksApp(uint16_t commandId, const uint16_t *payload,
                                 uint16_t payloadSize) {
  switch (commandId) {
    case DEVHOOKS_APP_DISKTEST:
      return devdisktest_request();
    case DEVHOOKS_APP_HEAP_HOLD: {
      uint32_t kb = (payloadSize >= 2u) ? payload[0] : 0u;
      if (kb == 0u) {
        while (devhooksHeldHeap != NULL) {
          DevhooksHeldBlock *next = devhooksHeldHeap->next;
          free(devhooksHeldHeap);
          devhooksHeldHeap = next;
        }
        DPRINTF("devhooks: heap hold released\n");
        return 1;
      }
      DevhooksHeldBlock *block = malloc(sizeof(DevhooksHeldBlock) + kb * 1024u);
      if (block != NULL) {
        block->next = devhooksHeldHeap;
        devhooksHeldHeap = block;
      }
      DPRINTF("devhooks: holding %lu KB more heap: %s\n", (unsigned long)kb,
              (block != NULL) ? "ok" : "refused");
      return (block != NULL) ? 1u : 0u;
    }
    default:
      return 0;
  }
}
#endif

// Keep answering the ST for ms milliseconds, so a command in flight is not
// left without its answer while a sentinel command waits to be seen.
static void emul_serviceFor(uint32_t ms) {
  absolute_time_t until = make_timeout_time_ms(ms);
  while (absolute_time_diff_us(get_absolute_time(), until) > 0) {
    chandler_loop();
  }
}

// After the ST's hello: the Mac ROM's next boot finds its floppies back in
// their drives, though its Restart or Shut Down ejected them.
static void emul_serveStBoot(void) {
  if (chandler_consumeStBoot()) macdisk_mountEmpty();
}

static void __not_in_flash_func(emul_pollTick)(void) {
  chandler_loop();
  emul_serveStBoot();
  select_poll();
}

// The cartridge's boot screen under TOS (target/atarist/src/main.s) asks for
// Booster when X is pressed or its countdown ends. The Mac ROM never runs the
// cartridge's code, so it never sends this.
static void emul_bootScreenCb(TransmissionProtocol *protocol,
                              uint16_t *payloadPtr) {
  (void)payloadPtr;
  if (protocol->command_id == EMUL_CMD_BOOSTER) keepActive = false;
}

void emul_start() {
  // The anatomy of an app or microfirmware is as follows:
  // - The driver code running in the remote device (the computer)
  // - the driver code running in the host device (the rp2040/rp2350)
  //
  // The driver code running in the remote device is responsible for:
  // 1. Perform the emulation of the device (ex: a ROM cartridge)
  // 2. Handle the communication with the host device
  // 3. Handle the configuration of the driver (ex: the ROM file to load)
  // 4. Handle the communication with the user (ex: the terminal)
  //
  // The driver code running in the host device is responsible for:
  // 1. Handle the communication with the remote device
  // 2. Handle the configuration of the driver (ex: the ROM file to load)
  // 3. Handle the communication with the user (ex: the terminal)
  //
  // Hence, we effectively have two drivers running in two different devices
  // with different architectures and capabilities.
  //
  // Please read the documentation to learn to use the communication protocol
  // between the two devices in the tprotocol.h file.
  //

  // 1. Check if the host device must be initialized to perform the emulation
  //    of the device, or start in setup/configuration mode
  SettingsConfigEntry *appMode =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_MODE);
  int appModeValue = APP_MODE_SETUP;  // Setup menu
  if (appMode == NULL) {
    DPRINTF(
        "APP_MODE_SETUP not found in the configuration. Using default value\n");
  } else {
    appModeValue = atoi(appMode->value);
    DPRINTF("Start emulation in mode: %i\n", appModeValue);
  }

  // 2. Initialiaze the normal operation of the app, unless the configuration
  // option says to start the config app Or a SELECT button is (or was) pressed
  // to start the configuration section of the app

  // In this example, the flow will always start the configuration app first
  // The ROM Emulator app for example will check here if the start directly
  // in emulation mode is needed or not

  // 3. If we are here, it means the app is not in emulation mode, but in
  // setup/configuration mode

  // As a rule of thumb, the remote device (the computer) driver code must
  // be copied to the RAM of the host device where the emulation will take
  // place.
  // The code is stored as an array in the target_firmware.h file
  //
  // Copy the terminal firmware to RAM
  COPY_FIRMWARE_TO_RAM((uint16_t *)target_firmware, target_firmware_length);
#if defined(_DEBUG) && (_DEBUG != 0)
  // The ST must see exactly the generated image.
  if (memcmp((const void *)&__rom_in_ram_start__, target_firmware,
             (size_t)target_firmware_length * sizeof(uint16_t)) != 0) {
    DPRINTF("ERROR: cartridge image in RAM does not match target_firmware\n");
  } else {
    DPRINTF("Cartridge image in RAM verified (%u words)\n",
            (unsigned)target_firmware_length);
  }
  // Nothing from a previous run may survive past the end of the image.
  {
    const uint8_t *window = (const uint8_t *)&__rom_in_ram_start__;
    size_t used = (size_t)target_firmware_length * sizeof(uint16_t);
    size_t leftovers = 0;
    for (size_t i = used; i < ROM_SIZE_BYTES * ROM_BANKS; i++) {
      if (window[i] != 0) leftovers++;
    }
    DPRINTF("Cartridge window after the image: %u non-zero bytes\n",
            (unsigned)leftovers);
  }
#endif

  // Initialize the cartridge ROM4 read engine. ROM4 reads are served entirely
  // by chained DMAs feeding the PIO TX FIFO — no CPU/IRQ involvement.
  // Without this engine the cartridge image is unreadable from the m68k,
  // so a failure here is fatal: panic instead of stumbling on with a half-
  // configured PIO/DMA setup.
  if (init_romemul(false) < 0) {
    panic("init_romemul failed: PIO/DMA claim or program load returned <0");
  }

  // Bring up the ROM3 command capture (PIO + DMA ring on GPIO 26) and the
  // command handler that polls the ring, parses the protocol, and dispatches
  // each command to the registered callbacks. commemul is similarly load-
  // bearing — without it the m68k can issue commands but the RP never sees
  // them, so any non-OK return is fatal.
  if (commemul_init() < 0) {
    panic("commemul_init failed: PIO/DMA claim or program load returned <0");
  }
  chandler_init();
  chandler_addCB(emul_bootScreenCb);
  // The Mac's disks. Every unit answers "no medium" until the SD card is up,
  // but the identity is there before the first answer.
  chandler_addCB(macdisk_commandCb);
  macdisk_init();

  // After this point, the remote computer can execute the code

  // 4. Configure the SELECT button before anything slow (the SD card, the
  // network), so a press is seen from here on; its edge interrupt catches one
  // made while a wait cannot poll. A short press restarts the RP. A press held
  // for SELECT_LONG_RESET is a factory reset: the global settings are erased
  // and the RP restarts into Booster, which then clears every app's settings.
  select_configure();
  select_setResetCallback(reset_device);
  select_setLongResetCallback(reset_deviceAndEraseFlash);

  // 5. Init the sd card
  // Most of the apps or microfirmwares will need to read and write files
  // to the SD card. The SD card is used to store the ROM, floppies, even
  // full hard disk files, configuration files, and other data.
  // The SD card is initialized here. If the SD card is not present, the
  // app continues without it: every disk unit stays empty.
  // Each app or microfirmware must have a folder in the SD card where the
  // files are stored. The folder name is defined in the configuration.
  // If there is no folder in the micro SD card, the app will create it.

  // Static, not on the stack: FatFs keeps a pointer to it while the card is
  // mounted, and it would take a quarter of core 0's stack.
  static FATFS fsys;
  SettingsConfigEntry *folder =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_FOLDER);
  char *folderName = "/jackintosh";
  if (folder == NULL) {
    DPRINTF("FOLDER not found in the configuration. Using default value\n");
  } else {
    DPRINTF("FOLDER: %s\n", folder->value);
    folderName = folder->value;
  }
  int sdcardErr = sdcard_initFilesystem(&fsys, folderName);
  if (sdcardErr != SDCARD_INIT_OK) {
    DPRINTF("SD card unavailable (error %i). Continuing without SD.\n",
            sdcardErr);
  } else {
    DPRINTF("SD card found & initialized\n");
    macdisk_mountAll(folderName);
    // Debug builds: the disk self-test, when its trigger file is on the card.
    devdisktest_runIfRequested(folderName);
  }

  // 6. Init the network, if needed
  // It's always a good idea to wait for the network to be ready
  // Get the WiFi mode from the settings
  // If you are developing code that does not use the network, you can
  // comment this section
  // It's important to note that the network parameters are taken from the
  // global configuration of the Booster app. The network parameters are
  // ready only for the microfirmware apps.
  SettingsConfigEntry *wifiMode =
      settings_find_entry(gconfig_getContext(), PARAM_WIFI_MODE);
  wifi_mode_t wifiModeValue = WIFI_MODE_STA;
  if (wifiMode == NULL) {
    DPRINTF("No WiFi mode found in the settings. No initializing.\n");
  } else {
    wifiModeValue = (wifi_mode_t)atoi(wifiMode->value);
    if (wifiModeValue != WIFI_MODE_AP) {
      DPRINTF("WiFi mode is STA\n");
      wifiModeValue = WIFI_MODE_STA;
      int err = network_wifiInit(wifiModeValue);
      if (err != 0) {
        DPRINTF("Error initializing the network: %i. No initializing.\n", err);
      } else {
        // Drain commands and run the terminal loop during WiFi polling so
        // commands sent during the (potentially multi-second) connect don't
        // pile up in the ROM3 ring.
        network_setPollingCallback(emul_pollTick);
        // Connect to the WiFi network
        int maxAttempts = 3;  // or any other number defined elsewhere
        int attempt = 0;
        err = NETWORK_WIFI_STA_CONN_ERR_TIMEOUT;

        while ((attempt < maxAttempts) &&
               (err == NETWORK_WIFI_STA_CONN_ERR_TIMEOUT)) {
          err = network_wifiStaConnect();
          attempt++;

          if ((err > 0) && (err < NETWORK_WIFI_STA_CONN_ERR_TIMEOUT)) {
            DPRINTF("Error connecting to the WiFi network: %i\n", err);
          }
        }

        if (err == NETWORK_WIFI_STA_CONN_ERR_TIMEOUT) {
          DPRINTF("Timeout connecting to the WiFi network after %d attempts\n",
                  maxAttempts);
          // Optionally, return an error code here.
        }
        network_setPollingCallback(NULL);
      }
    } else {
      DPRINTF("WiFi mode is AP. No initializing.\n");
    }
  }

  // Blink on
#ifdef BLINK_H
  blink_on();
#endif

  // 7. Start the main loop
  // The main loop serves the Mac's disks until the boot screen asks for
  // Booster.
  // Debug builds only: serve the SWD mailbox of tools/dev/swd.py
  devhooks_setAppHandler(emul_devhooksApp);

  DPRINTF("Start the app loop here\n");
  absolute_time_t nextNetworkPoll = get_absolute_time();
  while (keepActive) {
    devhooks_poll();
    select_poll();
    // Drain the ROM3 command ring and dispatch to the registered callbacks on
    // every pass: the ST spins on its answer, so the loop never waits.
    chandler_loop();
    emul_serveStBoot();
    macdisk_printAnswered();
    macdisk_syncPending();
    devdisktest_poll(folderName);
#if PICO_CYW43_ARCH_POLL
    // Wi-Fi every 10 ms, which is plenty for lwIP's timers and leaves the
    // loop to the command ring. Polling on every pass also works: a HardFault
    // inside cyw43_arch_poll() once blamed on it was the debug probe's tooling
    // writing into this firmware's RAM (fixed in tools/dev/swd.py).
    if (absolute_time_diff_us(nextNetworkPoll, get_absolute_time()) >= 0) {
      network_safePoll();
      ntp_pollSync();
      nextNetworkPoll = make_timeout_time_ms(10);
    }
#endif
  }

  // 8. Reset the ST and start Booster
  emul_serviceFor(SLEEP_LOOP_MS);
  // Hold the reset long enough for the boot screen to see it, and keep
  // answering: a command in flight would otherwise keep the ST in its send,
  // retrying, until the hold was over.
  DPRINTF("Sending command: %08x\n", EMUL_SENTINEL_RESET);
  WRITE_AND_SWAP_LONGWORD((uint8_t *)&__rom_in_ram_start__,
                          CHANDLER_CMD_SENTINEL_OFFSET, EMUL_SENTINEL_RESET);
  emul_serviceFor(SENTINEL_HOLD_MS);

  // Before jumping to the booster app, let's clean the settings
  // Set emulation mode to 255 (setup menu)
  settings_put_integer(aconfig_getContext(), ACONFIG_PARAM_MODE,
                       APP_MODE_SETUP);
  settings_save(aconfig_getContext(), true);

  // Jump to the booster app
  DPRINTF("Jumping to the booster app...\n");
  reset_jump_to_booster();
}
