/**
 * File: macdisk.c
 * Author: Diego Parrilla Santamaría
 * Date: October 2026
 * Copyright: 2026 - GOODDATA LABS SL
 * Description: The Mac's floppy and hard disk images, served from the microSD
 *              card as 512-byte block devices. See macdisk.h.
 */

#include "macdisk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "constants.h"
#include "debug.h"
#include "ff.h"
#include "memfunc.h"
#include "ntp.h"
#include "pico/stdlib.h"
#include "settings.h"

// Fastseek's cluster link map: the size tried first, and the most a fragmented
// image may take before it is served without one.
#define MACDISK_CLTBL_INITIAL 32u
#define MACDISK_CLTBL_MAX 512u

// What format detection reads from the start of the file: a MacBinary header
// and the DiskCopy 4.2 header behind it.
#define MACBINARY_HEADER_SIZE 128u
#define DC42_HEADER_SIZE 0x54u
#define MACDISK_PROBE_SIZE (MACBINARY_HEADER_SIZE + DC42_HEADER_SIZE)

// Seconds from 1904-01-01, the Mac's epoch, to 1970-01-01.
#define MACDISK_UNIX_TO_MAC 2082844800u

typedef struct {
  FIL file;
  DWORD *cltbl;
  FSIZE_t dataOffset;  // where sector 0 starts in the file
  uint32_t sectors;
  uint32_t changes;
  uint8_t format;
  bool open;
  bool readOnly;
  bool dirty;
  uint8_t syncFailures;
  uint32_t dirtyAtMs;
} MacdiskUnit;

static const char *const unitNames[MACDISK_UNITS] = {"FD0", "FD1", "HD0"};
static MacdiskUnit units[MACDISK_UNITS];

// Static, not on core 0's stack: a FILINFO with long names is ~280 bytes.
static FILINFO fileInfo;
static char unitPath[SETTINGS_MAX_VALUE_LENGTH + FF_LFN_BUF + 2];
static uint8_t probe[MACDISK_PROBE_SIZE];
// One write's data, or the PRAM, in the disk's byte order.
static uint8_t writeBuffer[MACDISK_MAX_WRITE_SECTORS * MACDISK_SECTOR_SIZE];

#if defined(_DEBUG) && (_DEBUG != 0)
// The commands answered since the main loop last printed them: printing
// inside the callback would delay the answer.
typedef struct {
  uint32_t atUs;
  uint32_t busyUs;
  uint32_t d3;
  uint32_t d4;
  int32_t result;
  uint16_t command;
} MacdiskLogEntry;
#define MACDISK_LOG_ENTRIES 64u
static MacdiskLogEntry commandLog[MACDISK_LOG_ENTRIES];
static uint32_t commandLogHead = 0;  // written by the callback
static uint32_t commandLogTail = 0;  // read by macdisk_printAnswered()
static uint32_t commandLogLost = 0;
#endif

static bool cardMounted = false;
static char mountFolder[SETTINGS_MAX_VALUE_LENGTH];
static char pramPath[SETTINGS_MAX_VALUE_LENGTH + sizeof(MACDISK_PRAM_FILE) + 1];
static FIL pramFile;

static inline uint8_t *macdisk_windowBase(void) {
  return (uint8_t *)&__rom_in_ram_start__;
}

static uint32_t macdisk_readBe32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | p[3];
}

static void macdisk_publishUnit(uint8_t unit) {
  const MacdiskUnit *u = &units[unit];
  uint32_t flags = 0;
  if (u->open) {
    flags |= MACDISK_FLAG_INSERTED;
    if (u->readOnly) flags |= MACDISK_FLAG_READONLY;
  }
  uint32_t base = MACDISK_UNIT_OFFSET(unit);
  uint8_t *window = macdisk_windowBase();
  WRITE_AND_SWAP_LONGWORD(window, base + MACDISK_UNIT_FLAGS, flags);
  WRITE_AND_SWAP_LONGWORD(window, base + MACDISK_UNIT_SECTORS,
                          u->open ? u->sectors : 0u);
  WRITE_AND_SWAP_LONGWORD(window, base + MACDISK_UNIT_CHANGES, u->changes);
  WRITE_AND_SWAP_LONGWORD(window, base + MACDISK_UNIT_FORMAT,
                          u->open ? u->format : MACDISK_FORMAT_NONE);
}

// A DiskCopy 4.2 header at hdr, in a file region of limit bytes from it.
static bool macdisk_isDc42(const uint8_t *hdr, FSIZE_t limit,
                           uint32_t *dataSize) {
  if (limit < DC42_HEADER_SIZE) return false;
  if (hdr[0] > 63) return false;  // the disk name, a Pascal string
  if (hdr[0x52] != 0x01 || hdr[0x53] != 0x00) return false;
  uint32_t data = macdisk_readBe32(hdr + 0x40);
  uint32_t tags = macdisk_readBe32(hdr + 0x44);
  if (data == 0 || (data % MACDISK_SECTOR_SIZE) != 0) return false;
  if ((uint64_t)DC42_HEADER_SIZE + data + tags > (uint64_t)limit) return false;
  *dataSize = data;
  return true;
}

// A MacBinary II header, whose data fork is a DiskCopy 4.2 image.
static bool macdisk_isMacBinaryDc42(const uint8_t *hdr, FSIZE_t size,
                                    uint32_t *dataSize) {
  if (size < MACDISK_PROBE_SIZE) return false;
  if (hdr[0] != 0 || hdr[74] != 0 || hdr[82] != 0) return false;
  if (hdr[1] == 0 || hdr[1] > 63) return false;  // the file name
  uint32_t fork = macdisk_readBe32(hdr + 83);
  if ((uint64_t)MACBINARY_HEADER_SIZE + fork > (uint64_t)size) return false;
  return macdisk_isDc42(hdr + MACBINARY_HEADER_SIZE, fork, dataSize);
}

static void macdisk_detectFormat(MacdiskUnit *u, UINT probed) {
  FSIZE_t size = f_size(&u->file);
  uint32_t dataSize = 0;
  if (probed == MACDISK_PROBE_SIZE &&
      macdisk_isMacBinaryDc42(probe, size, &dataSize)) {
    u->format = MACDISK_FORMAT_MACBINARY_DC42;
    u->dataOffset = MACBINARY_HEADER_SIZE + DC42_HEADER_SIZE;
    u->sectors = dataSize / MACDISK_SECTOR_SIZE;
    u->readOnly = true;
  } else if (probed >= DC42_HEADER_SIZE &&
             macdisk_isDc42(probe, size, &dataSize)) {
    u->format = MACDISK_FORMAT_DC42;
    u->dataOffset = DC42_HEADER_SIZE;
    u->sectors = dataSize / MACDISK_SECTOR_SIZE;
    u->readOnly = true;
  } else {
    u->format = MACDISK_FORMAT_RAW;
    u->dataOffset = 0;
    uint64_t sectors = (uint64_t)size / MACDISK_SECTOR_SIZE;
    u->sectors = (sectors > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)sectors;
  }
}

// Best effort: without the map f_lseek walks the FAT chain, which is slow on a
// large hard disk image but still correct.
static void macdisk_setupFastseek(MacdiskUnit *u) {
  DWORD *tbl = malloc(MACDISK_CLTBL_INITIAL * sizeof(DWORD));
  if (tbl == NULL) return;
  tbl[0] = MACDISK_CLTBL_INITIAL;
  u->file.cltbl = tbl;
  FRESULT fr = f_lseek(&u->file, CREATE_LINKMAP);
  if (fr == FR_NOT_ENOUGH_CORE) {
    DWORD needed = tbl[0];  // FatFs writes the size it needs here
    free(tbl);
    u->file.cltbl = NULL;
    if (needed == 0 || needed > MACDISK_CLTBL_MAX) {
      DPRINTF("macdisk: image too fragmented for fastseek (%lu DWORDs)\n",
              (unsigned long)needed);
      return;
    }
    tbl = malloc((size_t)needed * sizeof(DWORD));
    if (tbl == NULL) return;
    tbl[0] = needed;
    u->file.cltbl = tbl;
    fr = f_lseek(&u->file, CREATE_LINKMAP);
  }
  if (fr != FR_OK) {
    DPRINTF("macdisk: fastseek setup failed (%d)\n", (int)fr);
    u->file.cltbl = NULL;
    free(tbl);
    return;
  }
  u->cltbl = tbl;
}

static void macdisk_mountUnit(uint8_t unit, const char *folder) {
  MacdiskUnit *u = &units[unit];
  char pattern[8];
  snprintf(pattern, sizeof(pattern), "%s.*", unitNames[unit]);

  DIR dir;
  FRESULT fr = f_findfirst(&dir, &fileInfo, folder, pattern);
  while (fr == FR_OK && fileInfo.fname[0] != '\0' &&
         (fileInfo.fattrib & AM_DIR)) {
    fr = f_findnext(&dir, &fileInfo);
  }
  f_closedir(&dir);
  if (fr != FR_OK || fileInfo.fname[0] == '\0') {
    DPRINTF("macdisk: %s empty, no %s in %s\n", unitNames[unit], pattern,
            folder);
    return;
  }
  snprintf(unitPath, sizeof(unitPath), "%s/%s", folder, fileInfo.fname);

  u->readOnly = false;
  fr = f_open(&u->file, unitPath, FA_OPEN_EXISTING | FA_READ | FA_WRITE);
  if (fr == FR_DENIED || fr == FR_WRITE_PROTECTED) {
    u->readOnly = true;
    fr = f_open(&u->file, unitPath, FA_OPEN_EXISTING | FA_READ);
  }
  if (fr != FR_OK) {
    DPRINTF("macdisk: %s cannot open %s (%d)\n", unitNames[unit], unitPath,
            (int)fr);
    return;
  }

  UINT probed = 0;
  if (f_read(&u->file, probe, sizeof(probe), &probed) != FR_OK) probed = 0;
  macdisk_detectFormat(u, probed);
  if (u->sectors == 0) {
    DPRINTF("macdisk: %s %s holds no whole sector\n", unitNames[unit],
            unitPath);
    f_close(&u->file);
    return;
  }
  macdisk_setupFastseek(u);
  u->open = true;
  u->changes++;
  macdisk_publishUnit(unit);
  DPRINTF("macdisk: %s is %s, format %u, %lu sectors, %s\n", unitNames[unit],
          unitPath, (unsigned)u->format, (unsigned long)u->sectors,
          u->readOnly ? "read-only" : "read-write");
}

static bool macdisk_syncUnit(uint8_t unit) {
  MacdiskUnit *u = &units[unit];
  FRESULT fr = f_sync(&u->file);
  if (fr == FR_OK) {
    u->dirty = false;
    u->syncFailures = 0;
    return true;
  }
  DPRINTF("macdisk: %s sync failed (%d)\n", unitNames[unit], (int)fr);
  u->dirtyAtMs = to_ms_since_boot(get_absolute_time());
  if (++u->syncFailures >= MACDISK_SYNC_FAIL_MAX) {
    // The card is likely wedged: stop spending the main loop on it.
    DPRINTF("macdisk: %s gives up syncing after %u failures\n", unitNames[unit],
            (unsigned)u->syncFailures);
    u->dirty = false;
    u->syncFailures = 0;
  }
  return false;
}

// Check a transfer of count sectors from lba on unit.
static int32_t macdisk_checkTransfer(uint16_t unit, uint16_t count,
                                     uint16_t maxCount, uint32_t lba) {
  if (unit >= MACDISK_UNITS || count == 0 || count > maxCount) {
    return MACDISK_ERR_PARAM;
  }
  const MacdiskUnit *u = &units[unit];
  if (!u->open) return MACDISK_ERR_NOMEDIUM;
  if (lba >= u->sectors || count > u->sectors - lba) return MACDISK_ERR_RANGE;
  return MACDISK_OK;
}

static int32_t macdisk_readSectors(uint16_t unit, uint16_t count,
                                   uint32_t lba) {
  int32_t err =
      macdisk_checkTransfer(unit, count, MACDISK_MAX_READ_SECTORS, lba);
  if (err != MACDISK_OK) return err;
  MacdiskUnit *u = &units[unit];
  uint8_t *buffer = macdisk_windowBase() + MACDISK_BUFFER_OFFSET;
  UINT bytes = (UINT)count * MACDISK_SECTOR_SIZE;
  UINT done = 0;
  FRESULT fr =
      f_lseek(&u->file, u->dataOffset + (FSIZE_t)lba * MACDISK_SECTOR_SIZE);
  if (fr == FR_OK) fr = f_read(&u->file, buffer, bytes, &done);
  if (fr != FR_OK || done != bytes) {
    DPRINTF("macdisk: %s read %lu+%u failed (%d)\n", unitNames[unit],
            (unsigned long)lba, (unsigned)count, (int)fr);
    return MACDISK_ERR_IO;
  }
  CHANGE_ENDIANESS_BLOCK16(buffer, bytes);
  return MACDISK_OK;
}

static int32_t macdisk_writeSectors(uint16_t unit, uint16_t count, uint32_t lba,
                                    const uint16_t *data, uint32_t dataBytes) {
  int32_t err =
      macdisk_checkTransfer(unit, count, MACDISK_MAX_WRITE_SECTORS, lba);
  if (err != MACDISK_OK) return err;
  MacdiskUnit *u = &units[unit];
  if (u->readOnly) return MACDISK_ERR_READONLY;
  UINT bytes = (UINT)count * MACDISK_SECTOR_SIZE;
  if (dataBytes < bytes) return MACDISK_ERR_PARAM;
  COPY_AND_CHANGE_ENDIANESS_BLOCK16(data, writeBuffer, bytes);
  UINT done = 0;
  FRESULT fr =
      f_lseek(&u->file, u->dataOffset + (FSIZE_t)lba * MACDISK_SECTOR_SIZE);
  if (fr == FR_OK) fr = f_write(&u->file, writeBuffer, bytes, &done);
  if (fr != FR_OK || done != bytes) {
    DPRINTF("macdisk: %s write %lu+%u failed (%d)\n", unitNames[unit],
            (unsigned long)lba, (unsigned)count, (int)fr);
    return MACDISK_ERR_IO;
  }
  if (!u->dirty) {
    u->dirty = true;
    u->dirtyAtMs = to_ms_since_boot(get_absolute_time());
  }
  return MACDISK_OK;
}

static int32_t macdisk_ejectUnit(uint16_t unit) {
  if (unit >= MACDISK_UNITS) return MACDISK_ERR_PARAM;
  MacdiskUnit *u = &units[unit];
  if (!u->open) return MACDISK_OK;
  if (u->dirty) (void)macdisk_syncUnit(unit);
  f_close(&u->file);
  free(u->cltbl);
  u->cltbl = NULL;
  u->open = false;
  u->dirty = false;
  u->changes++;
  macdisk_publishUnit(unit);
  DPRINTF("macdisk: %s ejected\n", unitNames[unit]);
  return MACDISK_OK;
}

static int32_t macdisk_time(void) {
  uint32_t unixSeconds = 0;
  bool valid = ntp_getUnixTime(&unixSeconds);
  uint8_t *window = macdisk_windowBase();
  WRITE_AND_SWAP_LONGWORD(window, MACDISK_TIME_SECONDS_OFFSET,
                          valid ? unixSeconds + MACDISK_UNIX_TO_MAC : 0u);
  WRITE_AND_SWAP_LONGWORD(window, MACDISK_TIME_FLAGS_OFFSET,
                          valid ? MACDISK_TIME_FLAG_VALID : 0u);
  return MACDISK_OK;
}

static int32_t macdisk_readPram(void) {
  if (!cardMounted) return MACDISK_ERR_NOMEDIUM;
  FRESULT fr = f_open(&pramFile, pramPath, FA_OPEN_EXISTING | FA_READ);
  if (fr == FR_NO_FILE || fr == FR_NO_PATH) return MACDISK_ERR_NOMEDIUM;
  if (fr != FR_OK) {
    DPRINTF("macdisk: cannot open %s (%d)\n", pramPath, (int)fr);
    return MACDISK_ERR_IO;
  }
  int32_t result = MACDISK_OK;
  UINT done = 0;
  if (f_size(&pramFile) != MACDISK_PRAM_SIZE) {
    result = MACDISK_ERR_NOMEDIUM;
  } else if (f_read(&pramFile, writeBuffer, MACDISK_PRAM_SIZE, &done) !=
                 FR_OK ||
             done != MACDISK_PRAM_SIZE) {
    result = MACDISK_ERR_IO;
  }
  f_close(&pramFile);
  if (result == MACDISK_OK) {
    COPY_AND_CHANGE_ENDIANESS_BLOCK16(
        writeBuffer, macdisk_windowBase() + MACDISK_BUFFER_OFFSET,
        MACDISK_PRAM_SIZE);
  }
  return result;
}

static int32_t macdisk_writePram(const uint16_t *data, uint32_t dataBytes) {
  if (dataBytes != MACDISK_PRAM_SIZE) return MACDISK_ERR_PARAM;
  if (!cardMounted) return MACDISK_ERR_NOMEDIUM;
  COPY_AND_CHANGE_ENDIANESS_BLOCK16(data, writeBuffer, MACDISK_PRAM_SIZE);
  // In place, so a power cut leaves the old bytes or the new ones.
  FRESULT fr = f_open(&pramFile, pramPath, FA_OPEN_ALWAYS | FA_WRITE);
  if (fr != FR_OK) {
    DPRINTF("macdisk: cannot open %s (%d)\n", pramPath, (int)fr);
    return MACDISK_ERR_IO;
  }
  UINT done = 0;
  fr = f_write(&pramFile, writeBuffer, MACDISK_PRAM_SIZE, &done);
  if (fr == FR_OK && done != MACDISK_PRAM_SIZE) fr = FR_DISK_ERR;
  if (fr == FR_OK) fr = f_truncate(&pramFile);  // a longer file is cut
  FRESULT closed = f_close(&pramFile);          // and synced
  if (fr == FR_OK) fr = closed;
  if (fr != FR_OK) {
    DPRINTF("macdisk: %s write failed (%d)\n", pramPath, (int)fr);
    return MACDISK_ERR_IO;
  }
  return MACDISK_OK;
}

void macdisk_init(void) {
  WRITE_AND_SWAP_LONGWORD(macdisk_windowBase(), MACDISK_ID_OFFSET,
                          (uint32_t)MACDISK_ID);
}

void macdisk_mountAll(const char *folder) {
  snprintf(mountFolder, sizeof(mountFolder), "%s", folder);
  snprintf(pramPath, sizeof(pramPath), "%s/%s", folder, MACDISK_PRAM_FILE);
  cardMounted = true;
  for (uint8_t unit = 0; unit < MACDISK_UNITS; unit++) {
    macdisk_mountUnit(unit, folder);
  }
}

void macdisk_mountEmpty(void) {
  if (!cardMounted) return;
  for (uint8_t unit = 0; unit < MACDISK_UNITS; unit++) {
    if (!units[unit].open) macdisk_mountUnit(unit, mountFolder);
  }
}

void macdisk_unmountAll(void) {
  for (uint8_t unit = 0; unit < MACDISK_UNITS; unit++) {
    (void)macdisk_ejectUnit(unit);
  }
}

void macdisk_syncPending(void) {
  uint32_t now = to_ms_since_boot(get_absolute_time());
  for (uint8_t unit = 0; unit < MACDISK_UNITS; unit++) {
    const MacdiskUnit *u = &units[unit];
    if (u->dirty && (now - u->dirtyAtMs) >= MACDISK_SYNC_DELAY_MS) {
      (void)macdisk_syncUnit(unit);
    }
  }
}

void macdisk_commandCb(TransmissionProtocol *protocol, uint16_t *payloadPtr) {
  if ((protocol->command_id >> 8) != APP_MACDISK) return;

  // d3 = unit | count << 16, d4 = first sector, d5, then a write's data.
  // Commands without them read what the buffer holds, unused.
  uint32_t d3 = TPROTO_GET_PAYLOAD_PARAM32(payloadPtr);
  uint16_t unit = (uint16_t)(d3 & 0xFFFFu);
  uint16_t count = (uint16_t)(d3 >> 16);
  uint32_t lba = TPROTO_GET_NEXT32_PAYLOAD_PARAM32(payloadPtr);
  const uint32_t headerBytes = 16;  // token, d3, d4, d5
#if defined(_DEBUG) && (_DEBUG != 0)
  uint32_t startedUs = time_us_32();
#endif

  int32_t result;
  switch (protocol->command_id) {
    case MACDISK_CMD_READ:
      result = macdisk_readSectors(unit, count, lba);
      break;
    case MACDISK_CMD_WRITE: {
      uint32_t size = tprotocol_clamp_payload_size(protocol->payload_size);
      uint32_t dataBytes = (size > headerBytes) ? size - headerBytes : 0;
      // payloadPtr is at d4: d5 follows, then the data.
      result =
          macdisk_writeSectors(unit, count, lba, payloadPtr + 4, dataBytes);
      break;
    }
    case MACDISK_CMD_EJECT:
      result = macdisk_ejectUnit(unit);
      break;
    case MACDISK_CMD_TIME:
      result = macdisk_time();
      break;
    case MACDISK_CMD_PRAM_READ:
      result = macdisk_readPram();
      break;
    case MACDISK_CMD_PRAM_WRITE: {
      uint32_t size = tprotocol_clamp_payload_size(protocol->payload_size);
      uint32_t dataBytes = (size > headerBytes) ? size - headerBytes : 0;
      result = macdisk_writePram(payloadPtr + 4, dataBytes);
      break;
    }
    default:
      result = MACDISK_ERR_PARAM;
      break;
  }
  WRITE_AND_SWAP_LONGWORD(macdisk_windowBase(), MACDISK_RESULT_OFFSET,
                          (uint32_t)result);
#if defined(_DEBUG) && (_DEBUG != 0)
  if (commandLogHead - commandLogTail < MACDISK_LOG_ENTRIES) {
    MacdiskLogEntry *e = &commandLog[commandLogHead % MACDISK_LOG_ENTRIES];
    e->atUs = startedUs;
    e->busyUs = time_us_32() - startedUs;
    e->d3 = d3;
    e->d4 = lba;
    e->result = result;
    e->command = protocol->command_id;
    commandLogHead++;
  } else {
    commandLogLost++;
  }
#endif
}

void macdisk_printAnswered(void) {
#if defined(_DEBUG) && (_DEBUG != 0)
  static const char *const names[] = {"READ", "WRITE",     "EJECT",
                                      "TIME", "PRAM_READ", "PRAM_WRITE"};
  while (commandLogTail != commandLogHead) {
    const MacdiskLogEntry *e =
        &commandLog[commandLogTail % MACDISK_LOG_ENTRIES];
    uint8_t index = (uint8_t)(e->command & 0xFFu);
    DPRINTF(
        "macdisk: %lu.%03lu ms %04x %s unit %lu count %lu sector %lu -> %ld "
        "(%lu us)\n",
        (unsigned long)(e->atUs / 1000u), (unsigned long)(e->atUs % 1000u),
        (unsigned)e->command,
        index < sizeof(names) / sizeof(names[0]) ? names[index] : "?",
        (unsigned long)(e->d3 & 0xFFFFu), (unsigned long)(e->d3 >> 16),
        (unsigned long)e->d4, (long)e->result, (unsigned long)e->busyUs);
    commandLogTail++;
  }
  if (commandLogLost > 0) {
    DPRINTF("macdisk: %lu commands not logged (the log was full)\n",
            (unsigned long)commandLogLost);
    commandLogLost = 0;
  }
#endif
}
