/**
 * File: macdisk.h
 * Author: Diego Parrilla Santamaría
 * Date: October 2026
 * Copyright: 2026 - GOODDATA LABS SL
 * Description: The Mac's floppy and hard disk images, served from the microSD
 *              card as 512-byte block devices over the cartridge commands.
 */

#ifndef MACDISK_H
#define MACDISK_H

#include <stdbool.h>
#include <stdint.h>

#include "chandler.h"
#include "tprotocol.h"

// The client is the Mac ROM, which runs with no TOS: nothing on the ST runs
// this cartridge's own m68k code, so the commands below and the window layout
// they use are the whole contract. Commands are sent like send_sync and
// send_write_sync send them: the random token, then d3, d4 and d5.
//
// The images are chosen at the RP's power-on: in the app folder (FOLDER in the
// app settings), the first file named FD0.*, FD1.* and HD0.* is units 0, 1
// and 2. At every ST boot (CMD_ST_HELLO) a unit that is empty, because an
// EJECT emptied it or nothing was there, is looked up again the same way; a
// unit that holds its image keeps it. The format is told by the content, not
// the extension:
//   raw              the file is the disk, read-write unless the file is
//                    read-only on the card
//   DiskCopy 4.2     data from $54, read-only (writes would leave the header's
//                    checksum stale); the tags are not served
//   MacBinary II     around a DiskCopy 4.2 image (data fork from 128),
//                    read-only
//
// The Mac's 256 bytes of parameter RAM are kept in PRAM.BIN in the same
// folder, in the Mac's byte order.
//
// Every command writes its result (MACDISK_OK or a negative MACDISK_ERR_*) as
// a long at MACDISK_RESULT_OFFSET before it is answered. A retry is a new
// command, so each one is harmless to repeat: reads and writes name an
// absolute sector, an eject of an empty unit succeeds, the PRAM is read and
// written whole.

// The terminal is 0x00 and chandler's own 0xFF. 0x01-0x06 are taken in the
// other SidecarTridge firmwares (0x01 the legacy ROM emulator, 0x02 floppy,
// 0x03 RTC, 0x04 GEMDRIVE, 0x05 ACSI, 0x06 pool fix); 0x07 is used by none.
#define APP_MACDISK 0x07

// d3 = unit | count << 16, d4 = first sector. Reads count sectors into
// MACDISK_BUFFER_OFFSET.
#define MACDISK_CMD_READ ((APP_MACDISK << 8) | 0x00)
// d3 = unit | count << 16, d4 = first sector, d5 unused, then count * 512
// bytes of data (send_write_sync).
#define MACDISK_CMD_WRITE ((APP_MACDISK << 8) | 0x01)
// d3 = unit. Closes the image until the ST boots again (CMD_ST_HELLO).
#define MACDISK_CMD_EJECT ((APP_MACDISK << 8) | 0x02)
// No payload. Writes the time and its flags (MACDISK_TIME_*). The time is UTC:
// the Mac side applies its time zone.
#define MACDISK_CMD_TIME ((APP_MACDISK << 8) | 0x03)
// No payload. Copies the PRAM file into MACDISK_BUFFER_OFFSET. A missing file,
// or one that is not MACDISK_PRAM_SIZE bytes, is MACDISK_ERR_NOMEDIUM, and the
// buffer is left as it was.
#define MACDISK_CMD_PRAM_READ ((APP_MACDISK << 8) | 0x04)
// d3, d4, d5 unused (0), then MACDISK_PRAM_SIZE bytes of data
// (send_write_sync). The file is rewritten in place and synced before the
// answer.
#define MACDISK_CMD_PRAM_WRITE ((APP_MACDISK << 8) | 0x05)

#define MACDISK_UNIT_FD0 0  // Internal floppy
#define MACDISK_UNIT_FD1 1  // External floppy
#define MACDISK_UNIT_HD0 2  // Hard disk
#define MACDISK_UNITS 3

#define MACDISK_SECTOR_SIZE 512
#define MACDISK_MAX_READ_SECTORS 32  // 16 KB
// One send_write_sync payload: 16 bytes of token and registers, then the data,
// within MAX_PROTOCOL_PAYLOAD_SIZE.
#define MACDISK_MAX_WRITE_SECTORS 2
#define MACDISK_PRAM_SIZE 256
#define MACDISK_PRAM_FILE "PRAM.BIN"

// The status block, at the start of APP_FREE. Every field is a long, in the
// ST's byte order. The RP keeps the identity and the unit table current: the
// Mac reads them at any time without a command.
#define MACDISK_STATUS_OFFSET CHANDLER_APP_FREE_OFFSET
#define MACDISK_RESULT_OFFSET MACDISK_STATUS_OFFSET  // the last command's
// Written by MACDISK_CMD_TIME: UTC seconds since 1904-01-01 00:00, unsigned,
// and its flags. Both 0 while no NTP server has answered.
#define MACDISK_TIME_SECONDS_OFFSET (MACDISK_STATUS_OFFSET + 0x04)
#define MACDISK_TIME_FLAGS_OFFSET (MACDISK_STATUS_OFFSET + 0x08)
// MACDISK_ID, from the RP's boot on, card or not. Every app built on this
// framework answers every command, so the token alone does not say this one
// is running: after the hello, a high word other than MACDISK_ID_MAGIC, or an
// unknown version, means no disks.
#define MACDISK_ID_OFFSET (MACDISK_STATUS_OFFSET + 0x0C)
#define MACDISK_UNIT_TABLE_OFFSET (MACDISK_STATUS_OFFSET + 0x10)
#define MACDISK_UNIT_ENTRY_SIZE 0x10
#define MACDISK_UNIT_FLAGS 0x0    // MACDISK_FLAG_*
#define MACDISK_UNIT_SECTORS 0x4  // 512-byte sectors in the image
// Moves on every insert and eject. It restarts at 0 when the RP restarts, so
// compare it for a change, not for growth.
#define MACDISK_UNIT_CHANGES 0x8
#define MACDISK_UNIT_FORMAT 0xC  // MACDISK_FORMAT_*
#define MACDISK_UNIT_OFFSET(unit) \
  (MACDISK_UNIT_TABLE_OFFSET + (unit) * MACDISK_UNIT_ENTRY_SIZE)

// The microSD card, MACDISK_CARD_*, from the RP's boot on. Anything but
// MACDISK_CARD_OK means the firmware stopped there: no disks and no network,
// the LED blinking, until the RP restarts (power, or a short SELECT press).
// Every disk and PRAM command then answers MACDISK_ERR_NOMEDIUM.
#define MACDISK_CARD_OFFSET (MACDISK_STATUS_OFFSET + 0x40)

// Sector data, in the order the 68000 reads it.
#define MACDISK_BUFFER_OFFSET (MACDISK_STATUS_OFFSET + 0x100)
#define MACDISK_BUFFER_SIZE (MACDISK_MAX_READ_SECTORS * MACDISK_SECTOR_SIZE)

#define MACDISK_ID_MAGIC 0x4A4B  // "JK"
#define MACDISK_ID_VERSION 1     // bumped when this contract changes
#define MACDISK_ID ((MACDISK_ID_MAGIC << 16) | MACDISK_ID_VERSION)

#define MACDISK_TIME_FLAG_VALID 0x1  // the time is from an NTP server

#define MACDISK_CARD_OK 0
#define MACDISK_CARD_NONE 1        // no card, or one that does not answer
#define MACDISK_CARD_UNREADABLE 2  // no FAT or exFAT file system on it
#define MACDISK_CARD_NO_FOLDER 3   // the app folder cannot be created
#define MACDISK_CARD_PENDING 4     // not tried yet: wait for one of the above

#define MACDISK_FLAG_INSERTED 0x1
#define MACDISK_FLAG_READONLY 0x2

#define MACDISK_FORMAT_NONE 0
#define MACDISK_FORMAT_RAW 1
#define MACDISK_FORMAT_DC42 2
#define MACDISK_FORMAT_MACBINARY_DC42 3

#define MACDISK_OK 0
#define MACDISK_ERR_PARAM -1     // unknown command or unit, bad count or size
#define MACDISK_ERR_NOMEDIUM -2  // no image in the unit, no PRAM file, no card
#define MACDISK_ERR_RANGE -3     // past the last sector
#define MACDISK_ERR_READONLY -4  // write to a read-only image
#define MACDISK_ERR_IO -5        // the card failed

// A write is synced to the card this long after the first one not yet synced,
// so a burst pays the directory update once. A power cut loses at most this.
#define MACDISK_SYNC_DELAY_MS 3000
// Consecutive failed syncs after which a unit stops retrying.
#define MACDISK_SYNC_FAIL_MAX 5

/**
 * @brief Publish the identity, before the first command is answered.
 */
void macdisk_init(void);

/**
 * @brief Publish the card's state at MACDISK_CARD_OFFSET (MACDISK_CARD_*).
 *
 * macdisk_init() publishes MACDISK_CARD_PENDING and macdisk_mountAll()
 * MACDISK_CARD_OK; the app publishes why a card could not be used.
 */
void macdisk_setCardStatus(uint32_t status);

/**
 * @brief Mount the images of every unit from the app folder.
 *
 * Call once, after the SD card is mounted. A unit without an image, or with
 * one that cannot be opened, stays empty. Until this runs every unit is empty,
 * and the PRAM commands answer MACDISK_ERR_NOMEDIUM.
 */
void macdisk_mountAll(const char *folder);

/**
 * @brief Mount the empty units again from the folder of macdisk_mountAll().
 *
 * Call at every ST boot, once its hello is answered (chandler_consumeStBoot()):
 * a Mac's Restart or Shut Down ejects its floppies, and its next boot must
 * find them in their drives. Units that hold an image are left as they are.
 */
void macdisk_mountEmpty(void);

/**
 * @brief Eject every unit, syncing its pending writes first.
 *
 * For the debug self-test (devdisktest.h), which mounts its own images with
 * macdisk_mountAll() and puts the card's back the same way.
 */
void macdisk_unmountAll(void);

/**
 * @brief Sync the units whose writes have waited MACDISK_SYNC_DELAY_MS.
 *
 * Call from the main loop.
 */
void macdisk_syncPending(void);

/**
 * @brief Debug builds: print the commands answered since the last call, with
 * the RP's time each arrived, its registers, result and time to answer.
 *
 * Call from the main loop, after chandler_loop(): the callback only records
 * them, so the answer is not delayed. Release builds print nothing.
 */
void macdisk_printAnswered(void);

/**
 * @brief True once after the card was read or written for the Mac.
 *
 * For the activity LED, which the main loop pulses after the answer.
 */
bool macdisk_consumeCardActivity(void);

/**
 * @brief Command callback for chandler_addCB().
 */
void macdisk_commandCb(TransmissionProtocol *protocol, uint16_t *payloadPtr);

#endif  // MACDISK_H
