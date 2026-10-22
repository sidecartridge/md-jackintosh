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
#include <strings.h>

#include "aconfig.h"
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
  uint32_t imageId;  // MACDISK_UNIT_ID_OFFSET's hash of its path
  uint8_t format;
  bool open;
  bool readOnly;
  bool dirty;
  uint8_t syncFailures;
  uint32_t dirtyAtMs;
} MacdiskUnit;

static const char *const unitNames[MACDISK_UNITS] = {"FD0", "FD1", "HD0"};
static MacdiskUnit units[MACDISK_UNITS];

// The app settings of the MACDISK_KEY_* keys, in their order.
static const char *const settingKeys[MACDISK_KEYS] = {
    ACONFIG_PARAM_FOLDER,       ACONFIG_PARAM_FD0,
    ACONFIG_PARAM_FD0_2,        ACONFIG_PARAM_FD0_3,
    ACONFIG_PARAM_FD0_4,        ACONFIG_PARAM_FD0_5,
    ACONFIG_PARAM_FD0_6,        ACONFIG_PARAM_FD0_7,
    ACONFIG_PARAM_FD0_8,        ACONFIG_PARAM_FD0_9,
    ACONFIG_PARAM_FD0_10,       ACONFIG_PARAM_FD1,
    ACONFIG_PARAM_HD0,          ACONFIG_PARAM_RTC_ENABLED,
    ACONFIG_PARAM_RTC_NTP_HOST, ACONFIG_PARAM_RTC_NTP_PORT,
    ACONFIG_PARAM_ROM_OPTION_0, ACONFIG_PARAM_ROM_OPTION_1,
    ACONFIG_PARAM_ROM_OPTION_2, ACONFIG_PARAM_ROM_OPTION_3,
    ACONFIG_PARAM_ROM_OPTION_4, ACONFIG_PARAM_ROM_OPTION_5,
    ACONFIG_PARAM_ROM_OPTION_6, ACONFIG_PARAM_ROM_OPTION_7,
    ACONFIG_PARAM_ROM_OPTION_8, ACONFIG_PARAM_ROM_OPTION_9};
static uint8_t fd0Slot = 1;  // FD0's current slot

// LIST's last listing. It takes 26 KB, so it is allocated by the first LIST,
// and kept.
typedef struct {
  char name[MACDISK_PATH_SIZE];  // NUL-padded
  uint32_t flags;                // MACDISK_ENTRY_*
  uint32_t bytes;
} MacdiskListEntry;
static MacdiskListEntry *listing = NULL;
static uint32_t listingCount = 0;
static uint32_t listingFlags = 0;
static char listingPath[MACDISK_PATH_SIZE];  // "" while there is none
static bool listingWholeCard = false;

// Static, not on core 0's stack: a FILINFO with long names is ~280 bytes.
static FILINFO fileInfo;
static char unitPath[SETTINGS_MAX_VALUE_LENGTH + FF_LFN_BUF + 2];
// A path from LIST or SET_SETTING, and FOLDER as their top.
static char requestPath[MACDISK_PATH_SIZE] __attribute__((aligned(2)));
static char topFolder[MACDISK_PATH_SIZE];
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
static bool cardActivity = false;  // since macdisk_consumeCardActivity()
static char pramPath[SETTINGS_MAX_VALUE_LENGTH + sizeof(MACDISK_PRAM_FILE) + 1];
static FIL pramFile;

static inline uint8_t *macdisk_windowBase(void) {
  return (uint8_t *)&__rom_in_ram_start__;
}

static uint32_t macdisk_readBe32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | p[3];
}

static void macdisk_writeBe32(uint8_t *p, uint32_t value) {
  p[0] = (uint8_t)(value >> 24);
  p[1] = (uint8_t)(value >> 16);
  p[2] = (uint8_t)(value >> 8);
  p[3] = (uint8_t)value;
}

static const char *macdisk_setting(uint8_t key) {
  const SettingsConfigEntry *entry =
      settings_find_entry(aconfig_getContext(), settingKeys[key]);
  return (entry != NULL) ? entry->value : "";
}

// The setting that names unit's image: FD0's current slot, FD1 or HD0.
static uint8_t macdisk_unitKey(uint8_t unit) {
  if (unit == MACDISK_UNIT_FD0) return MACDISK_KEY_FD0_SLOT(fd0Slot);
  return (unit == MACDISK_UNIT_FD1) ? MACDISK_KEY_FD1 : MACDISK_KEY_HD0;
}

// md-drives-emulator's floppy rule: only a name ending in ".rw" is written.
static bool macdisk_isRw(const char *name) {
  size_t len = strlen(name);
  return len > 3 && strcasecmp(name + len - 3, ".rw") == 0;
}

// FNV-1a of an image's full path; 0 stands for an empty unit, so never 0.
static uint32_t macdisk_pathId(const char *path) {
  uint32_t hash = 0x811C9DC5u;
  for (const unsigned char *c = (const unsigned char *)path; *c != '\0'; c++) {
    hash ^= *c;
    hash *= 16777619u;
  }
  return hash != 0 ? hash : 1u;
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
  // Before the flags and the counter: whoever sees them move reads the new ID.
  WRITE_AND_SWAP_LONGWORD(window, MACDISK_UNIT_ID_OFFSET(unit),
                          u->open ? u->imageId : 0u);
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

// The first file named FD0.*, FD1.* or HD0.* in folder, into unitPath.
static bool macdisk_findByName(uint8_t unit, const char *folder) {
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
    return false;
  }
  snprintf(unitPath, sizeof(unitPath), "%s/%s", folder, fileInfo.fname);
  return true;
}

// Open unitPath as unit's image.
static void macdisk_openUnit(uint8_t unit) {
  MacdiskUnit *u = &units[unit];
  // A floppy not named *.rw is opened read-only, so nothing can write it.
  u->readOnly = (unit != MACDISK_UNIT_HD0) && !macdisk_isRw(unitPath);
  FRESULT fr = FR_DENIED;
  if (!u->readOnly) {
    fr = f_open(&u->file, unitPath, FA_OPEN_EXISTING | FA_READ | FA_WRITE);
  }
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
  u->imageId = macdisk_pathId(unitPath);
  u->open = true;
  u->changes++;
  macdisk_publishUnit(unit);
  DPRINTF("macdisk: %s is %s, format %u, %lu sectors, %s\n", unitNames[unit],
          unitPath, (unsigned)u->format, (unsigned long)u->sectors,
          u->readOnly ? "read-only" : "read-write");
}

// Mount the image unit's setting names; an empty setting leaves it empty.
// With byNameIn, the first <unit>.* in that folder instead (the self-test).
static void macdisk_mountUnit(uint8_t unit, const char *byNameIn) {
  if (byNameIn != NULL) {
    if (!macdisk_findByName(unit, byNameIn)) return;
  } else {
    const char *chosen = macdisk_setting(macdisk_unitKey(unit));
    if (chosen[0] == '\0') {
      DPRINTF("macdisk: %s empty, no image chosen\n", unitNames[unit]);
      return;
    }
    snprintf(unitPath, sizeof(unitPath), "%s", chosen);
  }
  macdisk_openUnit(unit);
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

// The path the ST sent after the registers, into requestPath: in the 68000's
// byte order, NUL-terminated within MACDISK_PATH_SIZE bytes.
static bool macdisk_takePath(const uint16_t *data, uint32_t dataBytes) {
  uint32_t bytes =
      (dataBytes < MACDISK_PATH_SIZE) ? dataBytes : MACDISK_PATH_SIZE;
  bytes &= ~1u;
  COPY_AND_CHANGE_ENDIANESS_BLOCK16(data, requestPath, bytes);
  return memchr(requestPath, '\0', bytes) != NULL;
}

// An absolute path with no empty, "." or ".." part, no backslash and no ':':
// FatFs follows ".." and takes a backslash for '/', and either would lead out
// of FOLDER. A trailing '/' is dropped.
static bool macdisk_checkPath(char *path) {
  size_t len = strlen(path);
  if (len == 0 || path[0] != '/' || strpbrk(path, "\\:") != NULL) return false;
  if (len > 1 && path[len - 1] == '/') path[--len] = '\0';
  for (const char *part = path + 1; len > 1;) {
    const char *slash = strchr(part, '/');
    size_t n = (slash != NULL) ? (size_t)(slash - part) : strlen(part);
    bool dots = part[0] == '.' && (n == 1 || (n == 2 && part[1] == '.'));
    if (n == 0 || dots) return false;
    if (slash == NULL) break;
    part = slash + 1;
  }
  return true;
}

// FOLDER as it is now, without a trailing '/', into topFolder.
static void macdisk_loadTopFolder(void) {
  snprintf(topFolder, sizeof(topFolder), "%s",
           macdisk_setting(MACDISK_KEY_FOLDER));
  size_t len = strlen(topFolder);
  if (len > 1 && topFolder[len - 1] == '/') topFolder[len - 1] = '\0';
  if (topFolder[0] == '\0') snprintf(topFolder, sizeof(topFolder), "/");
}

// path is top or inside it.
static bool macdisk_isInside(const char *path, const char *top) {
  size_t len = strlen(top);
  if (len == 1) return true;  // "/"
  return strncasecmp(path, top, len) == 0 &&
         (path[len] == '\0' || path[len] == '/');
}

static int32_t macdisk_pathResult(FRESULT fr) {
  if (fr == FR_OK) return MACDISK_OK;
  if (fr == FR_NO_FILE || fr == FR_NO_PATH || fr == FR_INVALID_NAME) {
    return MACDISK_ERR_PARAM;
  }
  DPRINTF("macdisk: %s: error %d\n", requestPath, (int)fr);
  return MACDISK_ERR_IO;
}

// md-drives-emulator's order: folders first, then names ignoring case.
static int macdisk_compareEntries(const void *a, const void *b) {
  const MacdiskListEntry *x = a;
  const MacdiskListEntry *y = b;
  bool xFolder = (x->flags & MACDISK_ENTRY_FOLDER) != 0;
  bool yFolder = (y->flags & MACDISK_ENTRY_FOLDER) != 0;
  if (xFolder != yFolder) return xFolder ? -1 : 1;
  return strcasecmp(x->name, y->name);
}

static int32_t macdisk_loadListing(const char *path, const char *top) {
  listingPath[0] = '\0';
  listingCount = 0;
  listingFlags = 0;
  if (listing == NULL) {
    listing = malloc(MACDISK_LIST_MAX * sizeof(MacdiskListEntry));
    if (listing == NULL) return MACDISK_ERR_IO;
  }
  DIR dir;
  FRESULT fr = f_opendir(&dir, path);
  if (fr != FR_OK) return macdisk_pathResult(fr);
  uint32_t sorted = 0;  // where the sorted entries start
  if (strcasecmp(path, top) != 0) {
    MacdiskListEntry *e = &listing[listingCount++];
    memset(e, 0, sizeof(*e));
    snprintf(e->name, sizeof(e->name), "..");
    e->flags = MACDISK_ENTRY_FOLDER;
    sorted = 1;
  }
  // A name's path: path, '/' unless path is "/", the name.
  size_t prefix = (path[1] == '\0') ? 1 : strlen(path) + 1;
  while ((fr = f_readdir(&dir, &fileInfo)) == FR_OK &&
         fileInfo.fname[0] != '\0') {
    if (fileInfo.fname[0] == '.') continue;
    if (listingCount == MACDISK_LIST_MAX) {
      listingFlags |= MACDISK_LIST_MORE;
      break;
    }
    MacdiskListEntry *e = &listing[listingCount++];
    memset(e->name, 0, sizeof(e->name));
    strncpy(e->name, fileInfo.fname, sizeof(e->name) - 1);
    bool folder = (fileInfo.fattrib & AM_DIR) != 0;
    e->flags = folder ? MACDISK_ENTRY_FOLDER
                      : (macdisk_isRw(fileInfo.fname) ? MACDISK_ENTRY_RW : 0u);
    if (prefix + strlen(fileInfo.fname) >= MACDISK_PATH_SIZE) {
      e->flags |= MACDISK_ENTRY_TOO_LONG;
    }
    if (folder) {
      e->bytes = 0;
    } else {
      e->bytes = (fileInfo.fsize > 0xFFFFFFFFu) ? 0xFFFFFFFFu
                                                : (uint32_t)fileInfo.fsize;
    }
  }
  f_closedir(&dir);
  if (fr != FR_OK) {
    listingCount = 0;
    return macdisk_pathResult(fr);
  }
  qsort(listing + sorted, listingCount - sorted, sizeof(MacdiskListEntry),
        macdisk_compareEntries);
  snprintf(listingPath, sizeof(listingPath), "%s", path);
  return MACDISK_OK;
}

static int32_t macdisk_list(uint32_t first, uint32_t options,
                            const uint16_t *data, uint32_t dataBytes) {
  if (!cardMounted) return MACDISK_ERR_NOMEDIUM;
  if (!macdisk_takePath(data, dataBytes) || !macdisk_checkPath(requestPath)) {
    return MACDISK_ERR_PARAM;
  }
  bool wholeCard = (options & MACDISK_LIST_WHOLE_CARD) != 0;
  if (wholeCard) {
    snprintf(topFolder, sizeof(topFolder), "/");
  } else {
    macdisk_loadTopFolder();
  }
  if (!macdisk_isInside(requestPath, topFolder)) return MACDISK_ERR_PARAM;
  if (first == 0 || wholeCard != listingWholeCard ||
      strcmp(requestPath, listingPath) != 0) {
    int32_t err = macdisk_loadListing(requestPath, topFolder);
    if (err != MACDISK_OK) return err;
    listingWholeCard = wholeCard;
  }
  if (first != 0 && first >= listingCount) return MACDISK_ERR_PARAM;
  uint32_t returned = listingCount - first;
  if (returned > MACDISK_LIST_PAGE) returned = MACDISK_LIST_PAGE;

  uint8_t *out = macdisk_windowBase() + MACDISK_BUFFER_OFFSET;
  macdisk_writeBe32(out + MACDISK_LIST_TOTAL, listingCount);
  macdisk_writeBe32(out + MACDISK_LIST_RETURNED, returned);
  macdisk_writeBe32(out + MACDISK_LIST_FLAGS, listingFlags);
  for (uint32_t i = 0; i < returned; i++) {
    const MacdiskListEntry *e = &listing[first + i];
    uint8_t *entry = out + MACDISK_LIST_ENTRIES + i * MACDISK_LIST_ENTRY_SIZE;
    memcpy(entry + MACDISK_ENTRY_NAME, e->name, MACDISK_PATH_SIZE);
    macdisk_writeBe32(entry + MACDISK_ENTRY_FLAGS, e->flags);
    macdisk_writeBe32(entry + MACDISK_ENTRY_BYTES, e->bytes);
  }
  CHANGE_ENDIANESS_BLOCK16(
      out, MACDISK_LIST_ENTRIES + returned * MACDISK_LIST_ENTRY_SIZE);
  return MACDISK_OK;
}

static int32_t macdisk_getSettings(void) {
  uint8_t *out = macdisk_windowBase() + MACDISK_BUFFER_OFFSET;
  for (uint8_t key = 0; key < MACDISK_KEYS; key++) {
    uint32_t offset;
    uint32_t size = MACDISK_PATH_SIZE;
    if (key < MACDISK_PATH_KEYS) {
      offset = key * MACDISK_PATH_SIZE;
    } else if (key < MACDISK_KEY_OPTION(0)) {
      offset = MACDISK_SETTINGS_RTC_OFFSET +
               (key - MACDISK_PATH_KEYS) * MACDISK_PATH_SIZE;
    } else {
      offset = MACDISK_SETTINGS_OPTION_FIELDS +
               (key - MACDISK_KEY_OPTION(0)) * MACDISK_OPTION_SIZE;
      size = MACDISK_OPTION_SIZE;
    }
    char *field = (char *)out + offset;
    memset(field, 0, size);
    strncpy(field, macdisk_setting(key), size - 1);
  }
  macdisk_writeBe32(out + MACDISK_SETTINGS_FD0_SLOT_OFFSET, fd0Slot);
  macdisk_writeBe32(out + MACDISK_SETTINGS_OPTIONS_OFFSET,
                    MACDISK_OPTIONS_MAGIC);
  CHANGE_ENDIANESS_BLOCK16(out, MACDISK_SETTINGS_SIZE);
  return MACDISK_OK;
}

// An RTC key's value, as macdisk.h's MACDISK_KEY_RTC_* say.
static bool macdisk_checkRtcValue(uint32_t key, const char *value) {
  size_t len = strlen(value);
  if (key == MACDISK_KEY_RTC_ENABLED) {
    return strcmp(value, "true") == 0 || strcmp(value, "false") == 0;
  }
  if (key == MACDISK_KEY_NTP_HOST) {
    const char *allowed =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-";
    return len > 0 && strspn(value, allowed) == len;
  }
  if (len == 0 || len > 5 || strspn(value, "0123456789") != len) return false;
  long port = strtol(value, NULL, 10);
  return port >= 1 && port <= 65535;
}

// A ROM option: printable ASCII, short enough for its field.
static bool macdisk_checkOptionValue(const char *value) {
  size_t len = strlen(value);
  if (len >= MACDISK_OPTION_SIZE) return false;
  for (size_t i = 0; i < len; i++) {
    if ((uint8_t)value[i] < 0x20 || (uint8_t)value[i] > 0x7E) return false;
  }
  return true;
}

// Stored with the type of its default, as md-drives-emulator stores them.
static int macdisk_putSetting(uint32_t key, const char *value) {
  SettingsContext *ctx = aconfig_getContext();
  if (key == MACDISK_KEY_RTC_ENABLED) {
    return settings_put_bool(ctx, settingKeys[key], strcmp(value, "true") == 0);
  }
  if (key == MACDISK_KEY_NTP_PORT) {
    return settings_put_integer(ctx, settingKeys[key], atoi(value));
  }
  return settings_put_string(ctx, settingKeys[key], value);
}

static int32_t macdisk_setSetting(uint32_t key, const uint16_t *data,
                                  uint32_t dataBytes) {
  if (key >= MACDISK_KEYS) return MACDISK_ERR_PARAM;
  if (!cardMounted) return MACDISK_ERR_NOMEDIUM;
  // The path, or the value of an RTC key or a ROM option.
  if (!macdisk_takePath(data, dataBytes)) return MACDISK_ERR_PARAM;
  if (key >= MACDISK_KEY_OPTION(0)) {
    if (!macdisk_checkOptionValue(requestPath)) return MACDISK_ERR_PARAM;
  } else if (key >= MACDISK_PATH_KEYS) {
    if (!macdisk_checkRtcValue(key, requestPath)) return MACDISK_ERR_PARAM;
  } else if (key == MACDISK_KEY_FOLDER) {
    if (!macdisk_checkPath(requestPath)) return MACDISK_ERR_PARAM;
    DIR dir;
    FRESULT fr = f_opendir(&dir, requestPath);
    if (fr != FR_OK) return macdisk_pathResult(fr);
    f_closedir(&dir);
  } else if (requestPath[0] != '\0') {
    if (!macdisk_checkPath(requestPath)) return MACDISK_ERR_PARAM;
    macdisk_loadTopFolder();
    if (!macdisk_isInside(requestPath, topFolder)) return MACDISK_ERR_PARAM;
    FRESULT fr = f_stat(requestPath, &fileInfo);
    if (fr != FR_OK) return macdisk_pathResult(fr);
    if (fileInfo.fattrib & AM_DIR) return MACDISK_ERR_PARAM;
  }

  // The flash is written only for a change: it takes ~45 ms.
  if (strcmp(macdisk_setting((uint8_t)key), requestPath) != 0) {
    if (macdisk_putSetting(key, requestPath) != 0 ||
        settings_save(aconfig_getContext(), true) != 0) {
      return MACDISK_ERR_IO;
    }
  }
  if (key == MACDISK_KEY_FOLDER) {
    listingPath[0] = '\0';  // its ".." may have changed
    return MACDISK_OK;
  }
  for (uint8_t unit = 0; unit < MACDISK_UNITS; unit++) {
    if (macdisk_unitKey(unit) != key) continue;
    (void)macdisk_ejectUnit(unit);
    if (requestPath[0] != '\0') {
      snprintf(unitPath, sizeof(unitPath), "%s", requestPath);
      macdisk_openUnit(unit);
    }
  }
  return MACDISK_OK;
}

void macdisk_init(void) {
  WRITE_AND_SWAP_LONGWORD(macdisk_windowBase(), MACDISK_ID_OFFSET,
                          (uint32_t)MACDISK_ID);
  macdisk_setCardStatus(MACDISK_CARD_PENDING);
}

void macdisk_setCardStatus(uint32_t status) {
  WRITE_AND_SWAP_LONGWORD(macdisk_windowBase(), MACDISK_CARD_OFFSET, status);
}

static void macdisk_mountFrom(const char *folder, bool byName) {
  snprintf(pramPath, sizeof(pramPath), "%s/%s", folder, MACDISK_PRAM_FILE);
  cardMounted = true;
  for (uint8_t unit = 0; unit < MACDISK_UNITS; unit++) {
    macdisk_mountUnit(unit, byName ? folder : NULL);
  }
  // Last: CARD_OK tells the Mac the unit table is current.
  macdisk_setCardStatus(MACDISK_CARD_OK);
}

void macdisk_mountAll(const char *folder) { macdisk_mountFrom(folder, false); }

void macdisk_mountByName(const char *folder) {
  macdisk_mountFrom(folder, true);
}

void macdisk_mountEmpty(void) {
  if (!cardMounted) return;
  for (uint8_t unit = 0; unit < MACDISK_UNITS; unit++) {
    if (!units[unit].open) macdisk_mountUnit(unit, NULL);
  }
}

bool macdisk_stepFd0(uint8_t *slot) {
  if (!cardMounted || units[MACDISK_UNIT_FD0].open) return false;
  for (uint8_t step = 1; step <= MACDISK_FD0_SLOTS; step++) {
    uint8_t next = (uint8_t)((fd0Slot - 1 + step) % MACDISK_FD0_SLOTS + 1);
    if (macdisk_setting(MACDISK_KEY_FD0_SLOT(next))[0] == '\0') continue;
    fd0Slot = next;
    *slot = next;
    macdisk_mountUnit(MACDISK_UNIT_FD0, NULL);
    return units[MACDISK_UNIT_FD0].open;
  }
  return false;
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
  // A send_write_sync's data, after the token, d3, d4 and d5. payloadPtr is
  // at d4: d5 follows, then the data.
  const uint32_t headerBytes = 16;
  uint32_t size = tprotocol_clamp_payload_size(protocol->payload_size);
  uint32_t dataBytes = (size > headerBytes) ? size - headerBytes : 0;
#if defined(_DEBUG) && (_DEBUG != 0)
  uint32_t startedUs = time_us_32();
#endif

  int32_t result;
  switch (protocol->command_id) {
    case MACDISK_CMD_READ:
      result = macdisk_readSectors(unit, count, lba);
      break;
    case MACDISK_CMD_WRITE:
      result =
          macdisk_writeSectors(unit, count, lba, payloadPtr + 4, dataBytes);
      break;
    case MACDISK_CMD_EJECT:
      result = macdisk_ejectUnit(unit);
      break;
    case MACDISK_CMD_TIME:
      result = macdisk_time();
      break;
    case MACDISK_CMD_PRAM_READ:
      result = macdisk_readPram();
      break;
    case MACDISK_CMD_PRAM_WRITE:
      result = macdisk_writePram(payloadPtr + 4, dataBytes);
      break;
    case MACDISK_CMD_LIST:
      result = macdisk_list(d3, lba, payloadPtr + 4, dataBytes);
      break;
    case MACDISK_CMD_GET_SETTINGS:
      result = macdisk_getSettings();
      break;
    case MACDISK_CMD_SET_SETTING:
      result = macdisk_setSetting(d3, payloadPtr + 4, dataBytes);
      break;
    default:
      result = MACDISK_ERR_PARAM;
      break;
  }
  WRITE_AND_SWAP_LONGWORD(macdisk_windowBase(), MACDISK_RESULT_OFFSET,
                          (uint32_t)result);
  if (protocol->command_id != MACDISK_CMD_TIME &&
      protocol->command_id != MACDISK_CMD_GET_SETTINGS) {
    cardActivity = true;
  }
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

bool macdisk_consumeCardActivity(void) {
  bool was = cardActivity;
  cardActivity = false;
  return was;
}

void macdisk_printAnswered(void) {
#if defined(_DEBUG) && (_DEBUG != 0)
  static const char *const names[] = {"READ", "WRITE",        "EJECT",
                                      "TIME", "PRAM_READ",    "PRAM_WRITE",
                                      "LIST", "GET_SETTINGS", "SET_SETTING"};
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
