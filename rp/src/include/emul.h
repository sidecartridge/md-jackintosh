/**
 * File: emul.h
 * Author: Diego Parrilla Santamaría
 * Date: January 20205, February 2026
 * Copyright: 2025-2026 - GOODDATA LABS SL
 * Description: Header for the ROM emulator core and setup features
 */

#ifndef EMUL_H
#define EMUL_H

// Sent by the cartridge's boot screen under TOS (CMD_BOOSTER in
// target/atarist/src/main.s), no payload: reset the ST and start Booster.
// App 0x00, the template terminal's number, which this app no longer uses.
#define EMUL_CMD_BOOSTER 0x0002

/**
 * @brief
 *
 * Launches the app: serves the cartridge image, mounts the Mac's disks from
 * the SD card, brings up the network, and runs the main loop until the boot
 * screen asks for Booster.
 */
void emul_start();

// App commands for `tools/dev/swd.py app NAME [WORDS...]`, debug builds only;
// the name after DEVHOOKS_APP_ is the one the tool takes.
//   heap_hold KB   hold KB more kilobytes of heap (0 releases everything);
//                  answers 0 when the allocation is refused, so repeated calls
//                  walk the heap down to a known remainder.
#define DEVHOOKS_APP_HEAP_HOLD 1
//   disktest       run the disk self-test (devdisktest.h) from the main loop;
//                  answers 1 when accepted, 0 while one is pending or runs.
//                  tools/dev/disktest_harness.py drives it and reads the
//                  outcome from devdisktestState.
#define DEVHOOKS_APP_DISKTEST 2

#endif  // EMUL_H
