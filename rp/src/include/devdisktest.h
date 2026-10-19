/**
 * File: devdisktest.h
 * Author: Diego Parrilla Santamaría
 * Date: October 2026
 * Copyright: 2026 - GOODDATA LABS SL
 * Description: Debug-only self-test of the Mac's disks on the device: real
 *              floppy and hard disk images on the card, read and written
 *              through macdisk.c's command path.
 */

#ifndef DEVDISKTEST_H
#define DEVDISKTEST_H

#include <stdint.h>

// The test never writes the card's own images. In DEVDISKTEST_FOLDER, under
// the app folder, it creates two sets of images filled with a pattern drawn
// from each sector's number, mounts each set in the three units in turn, and
// sends the units the commands the Mac ROM sends, as macdisk_commandCb()
// receives them from chandler. Set A is writable: an 800K and a 400K raw
// floppy and a fragmented raw hard disk. Set B is read-only: a MacBinary II
// DiskCopy 4.2 800K floppy, a DiskCopy 4.2 400K floppy and a raw hard disk
// marked read-only on the card. Then it mounts the card's own images again.
//
// It runs either at the RP's boot, when DEVDISKTEST_TRIGGER is in the app
// folder (the file is deleted first, so a test that hangs runs once), or when
// the host sends the app command DEVHOOKS_APP_DISKTEST (emul.h) through
// tools/dev/disktest_harness.py. Each check is a line in DEVDISKTEST_REPORT in
// the app folder and on the debug console; the totals and the command times
// are also in devdisktestState, found by its symbol. Nothing answers the ST
// while it runs. In release builds every call compiles to nothing.

#define DEVDISKTEST_MAGIC 0x54534B44u  // "DKST"
#define DEVDISKTEST_FOLDER "SELFTEST"
#define DEVDISKTEST_TRIGGER "SELFTEST.RUN"
#define DEVDISKTEST_REPORT "SELFTEST.LOG"
#define DEVDISKTEST_FAILURE_SIZE 96u

typedef enum {
  DEVDISKTEST_IDLE = 0,
  DEVDISKTEST_REQUESTED,
  DEVDISKTEST_RUNNING,
  DEVDISKTEST_DONE
} devdisktest_state_t;

typedef struct {
  uint32_t magic;
  uint32_t seq;        // incremented by every run
  uint32_t state;      // devdisktest_state_t
  uint32_t checks;     // checks run so far
  uint32_t failures;   // of them, failed
  uint32_t elapsedMs;  // the whole run, images included
  // Time macdisk_commandCb() took, which is how long the ST waits for the
  // answer past the command's own transfer: 32-sector reads, 2-sector writes,
  // and every command.
  uint32_t read32Count;
  uint32_t read32MaxUs;
  uint32_t read32TotalUs;
  uint32_t write2Count;
  uint32_t write2MaxUs;
  uint32_t write2TotalUs;
  uint32_t commandMaxUs;
  uint32_t clusterBytes;  // of the card, as FatFs mounted it
  char firstFailure[DEVDISKTEST_FAILURE_SIZE];
} DevdisktestState;

#if defined(_DEBUG) && (_DEBUG != 0)
// At boot, after macdisk_mountAll(folder): runs the test if the trigger file
// is there.
void devdisktest_runIfRequested(const char *folder);
// The app command: 1 when accepted, 0 while a run is pending or running.
uint32_t devdisktest_request(void);
// From the main loop: runs a requested test.
void devdisktest_poll(const char *folder);
#else
static inline void devdisktest_runIfRequested(const char *folder) {
  (void)folder;
}
static inline uint32_t devdisktest_request(void) { return 0; }
static inline void devdisktest_poll(const char *folder) { (void)folder; }
#endif

#endif  // DEVDISKTEST_H
