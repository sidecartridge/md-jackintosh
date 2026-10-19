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
// The images are the ROM's choice, kept in the app settings as full paths
// (MACDISK_CMD_SET_SETTING): FD0 has MACDISK_FD0_SLOTS slots, one of them
// current, and FD1 and HD0 one each. At the RP's power-on FD0 holds slot 1,
// FD1 and HD0 their own; a unit whose setting is empty stays empty. At every
// ST boot (CMD_ST_HELLO) a unit that is empty, because an EJECT emptied it or
// nothing was there, is mounted again the same way, FD0 from its current
// slot; a unit that holds its image keeps it. The current slot survives an ST
// boot and is slot 1 again at the RP's power-on.
//
// A floppy (FD0, FD1) is read-only unless its file name ends in ".rw", in
// any case, as in md-drives-emulator. The format is told by the content, not
// the extension:
//   raw              the file is the disk, read-write unless the file is
//                    read-only on the card (or a floppy without ".rw")
//   DiskCopy 4.2     data from $54, read-only (writes would leave the header's
//                    checksum stale); the tags are not served
//   MacBinary II     around a DiskCopy 4.2 image (data fork from 128),
//                    read-only
//
// The Mac's 256 bytes of parameter RAM are kept in PRAM.BIN in the app
// folder (FOLDER in the app settings, as it was at the RP's power-on), in the
// Mac's byte order.
//
// Paths and names are bytes in FatFs's code page 437, NUL-terminated, at most
// MACDISK_PATH_SIZE bytes with the NUL. A path from the ST is absolute, with
// no empty, "." or ".." part, no backslash and no ':'; a trailing '/' is
// dropped.
//
// Every command writes its result (MACDISK_OK or a negative MACDISK_ERR_*) as
// a long at MACDISK_RESULT_OFFSET before it is answered. A retry is a new
// command, so each one is harmless to repeat: reads and writes name an
// absolute sector, an eject of an empty unit succeeds, the PRAM and a setting
// are written whole.

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
// d3 = the first entry, d4 = MACDISK_LIST_WHOLE_CARD or 0, d5 unused (0),
// then a folder's path (send_write_sync). Lists the folder into
// MACDISK_BUFFER_OFFSET as MACDISK_LIST_* says: ".." first when the folder is
// below FOLDER, then the folders, then the files, each group sorted ignoring
// case. Names starting with "." are left out. MACDISK_ERR_PARAM for a path
// that is not a folder or is above FOLDER, or a first entry past the last;
// MACDISK_ERR_NOMEDIUM without a card. With MACDISK_LIST_WHOLE_CARD, for
// choosing FOLDER itself, "/" takes FOLDER's place in both rules. The listing
// is made again when the first entry is 0, or the folder or d4 changes; other
// pages of the same folder come from it.
#define MACDISK_CMD_LIST ((APP_MACDISK << 8) | 0x06)
// No payload. Copies the settings into MACDISK_BUFFER_OFFSET, in
// MACDISK_SETTINGS_SIZE bytes: for each path key (MACDISK_KEY_FOLDER to
// MACDISK_KEY_HD0, in order) its path in MACDISK_PATH_SIZE bytes, "" for
// none; then FD0's current slot as a long (MACDISK_SETTINGS_FD0_SLOT_OFFSET);
// then the RTC keys' values, MACDISK_PATH_SIZE bytes each, from
// MACDISK_SETTINGS_RTC_OFFSET. Answered without a card too: the settings are
// in the RP's flash.
#define MACDISK_CMD_GET_SETTINGS ((APP_MACDISK << 8) | 0x07)
// d3 = the key (MACDISK_KEY_*), d4, d5 unused (0), then the path, or an RTC
// key's value (send_write_sync); "" empties an image key. Checked first:
// FOLDER must be a folder, any other path key a file, inside FOLDER (FOLDER
// inside "/"); an RTC key's value as MACDISK_KEY_RTC_* says. Otherwise
// MACDISK_ERR_PARAM and nothing changes. Then saved to the settings flash,
// and for FD0's current slot, FD1 and HD0 the unit's image is swapped before
// the answer: the old one synced and closed, the new one opened (its changes
// count moves); "" leaves the unit empty. MACDISK_OK says it was saved; the
// unit table says what the unit holds. FOLDER is the top of LIST and of these
// checks at once, PRAM.BIN's folder from the RP's next power-on. The RTC keys
// apply from the RP's next start (power-on or restart).
// MACDISK_ERR_NOMEDIUM without a card, and nothing is saved.
#define MACDISK_CMD_SET_SETTING ((APP_MACDISK << 8) | 0x08)

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

// A path and its NUL: the length of a value in the app settings.
#define MACDISK_PATH_SIZE 96
#define MACDISK_FD0_SLOTS 10

// The keys of GET_SETTINGS and SET_SETTING.
#define MACDISK_KEY_FOLDER 0
#define MACDISK_KEY_FD0_SLOT(n) (n)  // n = 1 to MACDISK_FD0_SLOTS
#define MACDISK_KEY_FD1 11
#define MACDISK_KEY_HD0 12
#define MACDISK_PATH_KEYS 13  // keys 0 to 12 hold paths
// The RTC, md-drives-emulator's: the time from an NTP server. Off, the RP does
// not bring Wi-Fi up (the network is only for NTP), and TIME never gets a
// valid time: the Mac keeps its own clock.
#define MACDISK_KEY_RTC_ENABLED 13  // "true" (the default) or "false"
// A name or a dotted IPv4 address: letters, digits, '.' and '-'. The default
// is "pool.ntp.org".
#define MACDISK_KEY_NTP_HOST 14
#define MACDISK_KEY_NTP_PORT 15  // 1 to 65535 in decimal; the default is "123"
#define MACDISK_KEYS 16
#define MACDISK_SETTINGS_FD0_SLOT_OFFSET (MACDISK_PATH_KEYS * MACDISK_PATH_SIZE)
#define MACDISK_SETTINGS_RTC_OFFSET (MACDISK_SETTINGS_FD0_SLOT_OFFSET + 4)
#define MACDISK_SETTINGS_SIZE    \
  (MACDISK_SETTINGS_RTC_OFFSET + \
   (MACDISK_KEYS - MACDISK_PATH_KEYS) * MACDISK_PATH_SIZE)

// LIST's answer: three longs, then the entries.
#define MACDISK_LIST_MAX 256       // entries in a listing, ".." included
#define MACDISK_LIST_PAGE 16       // entries in one answer
#define MACDISK_LIST_TOTAL 0x0     // entries in the listing
#define MACDISK_LIST_RETURNED 0x4  // entries in this answer
#define MACDISK_LIST_FLAGS 0x8     // MACDISK_LIST_MORE
#define MACDISK_LIST_ENTRIES 0xC
#define MACDISK_LIST_ENTRY_SIZE 104
#define MACDISK_ENTRY_NAME 0x0    // MACDISK_PATH_SIZE bytes, NUL-terminated
#define MACDISK_ENTRY_FLAGS 0x60  // MACDISK_ENTRY_*
#define MACDISK_ENTRY_BYTES 0x64  // the file's size, at most $FFFFFFFF
#define MACDISK_LIST_MORE 0x1     // the folder holds more than MACDISK_LIST_MAX
#define MACDISK_LIST_WHOLE_CARD 0x1  // LIST's d4: from "/", not from FOLDER
#define MACDISK_ENTRY_FOLDER 0x1
#define MACDISK_ENTRY_RW 0x2  // a file named *.rw: writable in a floppy unit
// Its path would not fit MACDISK_PATH_SIZE: the name is cut short and a
// SET_SETTING or LIST with it fails.
#define MACDISK_ENTRY_TOO_LONG 0x4

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

// Sector data and the other commands' answers, in the order the 68000 reads
// them.
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
#define MACDISK_ERR_PARAM -1     // bad command, unit, key, count, size or path
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
 * @brief Mount the images the settings name in every unit.
 *
 * Call once, after the SD card is mounted. A unit with no image chosen, or
 * with one that cannot be opened, stays empty. Until this runs every unit is
 * empty, and the PRAM commands answer MACDISK_ERR_NOMEDIUM. PRAM.BIN is in
 * folder, the app folder.
 */
void macdisk_mountAll(const char *folder);

/**
 * @brief Like macdisk_mountAll(), with the first FD0.*, FD1.* and HD0.* in
 * folder instead of the settings' images.
 *
 * For the debug self-test (devdisktest.h), which mounts its own images this
 * way and puts the card's back with macdisk_mountAll().
 */
void macdisk_mountByName(const char *folder);

/**
 * @brief Mount the empty units again, as macdisk_mountAll() did.
 *
 * Call at every ST boot, once its hello is answered (chandler_consumeStBoot()):
 * a Mac's Restart or Shut Down ejects its floppies, and its next boot must
 * find them in their drives. Units that hold an image are left as they are.
 * FD0 is looked up from its current slot.
 */
void macdisk_mountEmpty(void);

/**
 * @brief Put FD0's next slot in the drive, if the drive is empty.
 *
 * The slots after the current one are tried in turn, wrapping, up to the
 * current one itself; the first that names an image becomes current and its
 * image is mounted. For a short SELECT press during a Mac session.
 *
 * @param slot Set to the slot that became current, if one did.
 * @return true if FD0 now holds that slot's image; false if FD0 was not
 * empty, no slot names an image, or the image could not be opened.
 */
bool macdisk_stepFd0(uint8_t *slot);

/**
 * @brief Eject every unit, syncing its pending writes first.
 *
 * For the debug self-test (devdisktest.h), which mounts its own images with
 * macdisk_mountByName() and puts the card's back with macdisk_mountAll().
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
