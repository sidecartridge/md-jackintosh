/**
 * File: devdisktest.c
 * Author: Diego Parrilla Santamaría
 * Date: October 2026
 * Copyright: 2026 - GOODDATA LABS SL
 * Description: Debug-only self-test of the Mac's disks on the device. See
 *              devdisktest.h.
 */

#include "devdisktest.h"

#if defined(_DEBUG) && (_DEBUG != 0)

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "constants.h"
#include "debug.h"
#include "ff.h"
#include "macdisk.h"
#include "memfunc.h"
#include "pico/stdlib.h"
#include "sdcard.h"
#include "settings.h"

__attribute__((used)) DevdisktestState devdisktestState = {
    .magic = DEVDISKTEST_MAGIC};

#define DT_SECTOR MACDISK_SECTOR_SIZE
#define DT_FD800_SECTORS 1600u
#define DT_FD400_SECTORS 800u
#define DT_HD_SECTORS 16384u      // 8 MB, cut into about DT_FRAGMENTS pieces
#define DT_HD_RO_SECTORS 2048u    // 1 MB
#define DT_FRAGMENTS 64u          // within macdisk.c's fastseek limit
#define DT_DC42_TAG_BYTES 19200u  // 12 bytes per sector of an 800K disk
#define DT_RANDOM_WRITES 8u
#define DT_MAX_WRITES (DT_RANDOM_WRITES + 6u)
#define DT_MARKER 0x7FFF7FFFu  // in the result slot before a command
#define MACBINARY_HEADER_SIZE 128u
#define DC42_HEADER_SIZE 0x54u

typedef struct {
  const char *name;
  uint32_t sectors;
  uint32_t seed;
  uint32_t format;  // MACDISK_FORMAT_*
  uint32_t dataOffset;
  uint32_t tagBytes;
  bool readOnly;
} DtImage;

typedef struct {
  uint32_t lba;
  uint32_t count;
  uint32_t seed;
} DtWrite;

static const char *const dtUnitNames[MACDISK_UNITS] = {"FD0", "FD1", "HD0"};

// Set A, writable: the floppies a Plus takes, and a fragmented hard disk.
static const DtImage dtSetA[MACDISK_UNITS] = {
    {"FD0.IMG", DT_FD800_SECTORS, 0xA0F00000u, MACDISK_FORMAT_RAW, 0, 0, false},
    {"FD1.IMG", DT_FD400_SECTORS, 0xA0F10000u, MACDISK_FORMAT_RAW, 0, 0, false},
    {"HD0.IMG", DT_HD_SECTORS, 0xA0D00000u, MACDISK_FORMAT_RAW, 0, 0, false},
};
// Set B, read-only: the DiskCopy formats, and a raw image marked read-only.
static const DtImage dtSetB[MACDISK_UNITS] = {
    {"FD0.IMAGE", DT_FD800_SECTORS, 0xB0F00000u, MACDISK_FORMAT_MACBINARY_DC42,
     MACBINARY_HEADER_SIZE + DC42_HEADER_SIZE, DT_DC42_TAG_BYTES, true},
    {"FD1.DC42", DT_FD400_SECTORS, 0xB0F10000u, MACDISK_FORMAT_DC42,
     DC42_HEADER_SIZE, 0, true},
    {"HD0.IMG", DT_HD_RO_SECTORS, 0xB0D00000u, MACDISK_FORMAT_RAW, 0, 0, true},
};

// Static, not on core 0's stack.
static TransmissionProtocol frame;
static uint8_t chunk[4096];
static uint8_t data[MACDISK_MAX_WRITE_SECTORS * DT_SECTOR];
static char path[SETTINGS_MAX_VALUE_LENGTH + 40];
static char setA[SETTINGS_MAX_VALUE_LENGTH + 16];
static char setB[SETTINGS_MAX_VALUE_LENGTH + 16];
static FIL file;
static FIL filler;
static FIL report;
static bool reportOpen = false;
static DtWrite writes[MACDISK_UNITS][DT_MAX_WRITES];
static uint32_t writeCount[MACDISK_UNITS];
static uint32_t hdPieceSectors = 0;  // set A's hard disk fragment length

// ---------------------------------------------------------------------------
// The pattern: every byte follows from the image's seed, the sector number and
// the byte's offset, so a sector read anywhere can be checked without a copy.

static uint8_t dt_pattern(uint32_t seed, uint32_t lba, uint32_t i) {
  uint32_t x = seed ^ (lba * 0x9E3779B1u) ^ ((i >> 2) * 0x85EBCA77u);
  x ^= x >> 15;
  x *= 0x2C1B3C6Du;
  x ^= x >> 12;
  x *= 0x297A2D39u;
  x ^= x >> 15;
  return (uint8_t)(x >> (8 * (i & 3)));
}

static void dt_fillSector(uint8_t *dst, uint32_t seed, uint32_t lba) {
  for (uint32_t i = 0; i < DT_SECTOR; i++) dst[i] = dt_pattern(seed, lba, i);
}

// The seed a sector of unit holds now: the image's, or its last write's.
static uint32_t dt_seedOf(uint8_t unit, const DtImage *img, uint32_t lba) {
  uint32_t seed = img->seed;
  for (uint32_t k = 0; k < writeCount[unit]; k++) {
    const DtWrite *w = &writes[unit][k];
    if (lba >= w->lba && lba < w->lba + w->count) seed = w->seed;
  }
  return seed;
}

// ---------------------------------------------------------------------------
// The report.

static void dt_line(const char *fmt, ...) {
  char line[160];
  va_list args;
  va_start(args, fmt);
  vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  DPRINTF("disktest: %s\n", line);
  if (reportOpen) {
    UINT written = 0;
    f_write(&report, line, (UINT)strlen(line), &written);
    f_write(&report, "\n", 1, &written);
    f_sync(&report);
  }
}

static void dt_check(bool ok, const char *fmt, ...) {
  char text[140];
  va_list args;
  va_start(args, fmt);
  vsnprintf(text, sizeof(text), fmt, args);
  va_end(args);
  devdisktestState.checks++;
  if (!ok) {
    if (devdisktestState.failures == 0) {
      snprintf(devdisktestState.firstFailure, DEVDISKTEST_FAILURE_SIZE, "%s",
               text);
    }
    devdisktestState.failures++;
  }
  dt_line("%s %s", ok ? "PASS" : "FAIL", text);
}

// ---------------------------------------------------------------------------
// The window as the 68000 reads it, and the commands as chandler hands them
// over.

static uint8_t *dt_window(void) { return (uint8_t *)&__rom_in_ram_start__; }

static uint8_t dt_stByte(uint32_t offset) {
  uint16_t word = ((const uint16_t *)dt_window())[offset / 2];
  return (offset & 1) ? (uint8_t)(word & 0xFF) : (uint8_t)(word >> 8);
}

static uint32_t dt_stLong(uint32_t offset) {
  return ((uint32_t)dt_stByte(offset) << 24) |
         ((uint32_t)dt_stByte(offset + 1) << 16) |
         ((uint32_t)dt_stByte(offset + 2) << 8) | dt_stByte(offset + 3);
}

static uint32_t dt_unitField(uint8_t unit, uint32_t field) {
  return dt_stLong(MACDISK_UNIT_OFFSET(unit) + field);
}

// One command, built as send_sync / send_write_sync send it: the token, d3,
// d4, d5, then the data in the 68000's words. Returns the result long.
static int32_t dt_command(uint16_t command, uint32_t d3, uint32_t d4,
                          const uint8_t *bytes, uint32_t length) {
  memset(&frame, 0, sizeof(frame));
  frame.command_id = command;
  const uint32_t regs[4] = {0x5A5A1234u, d3, d4, 0};
  for (int i = 0; i < 4; i++) {
    frame.payload[2 * i] = (uint16_t)(regs[i] & 0xFFFFu);
    frame.payload[2 * i + 1] = (uint16_t)(regs[i] >> 16);
  }
  for (uint32_t i = 0; i + 1 < length; i += 2) {
    frame.payload[8 + i / 2] = (uint16_t)((bytes[i] << 8) | bytes[i + 1]);
  }
  frame.payload_size = (uint16_t)(16 + length);
  WRITE_AND_SWAP_LONGWORD(dt_window(), MACDISK_RESULT_OFFSET, DT_MARKER);

  uint32_t startUs = time_us_32();
  macdisk_commandCb(&frame, frame.payload + 2);
  uint32_t us = time_us_32() - startUs;

  DevdisktestState *st = &devdisktestState;
  if (us > st->commandMaxUs) st->commandMaxUs = us;
  uint32_t count = d3 >> 16;
  if (command == MACDISK_CMD_READ && count == MACDISK_MAX_READ_SECTORS) {
    st->read32Count++;
    st->read32TotalUs += us;
    if (us > st->read32MaxUs) st->read32MaxUs = us;
  } else if (command == MACDISK_CMD_WRITE &&
             count == MACDISK_MAX_WRITE_SECTORS) {
    st->write2Count++;
    st->write2TotalUs += us;
    if (us > st->write2MaxUs) st->write2MaxUs = us;
  }
  return (int32_t)dt_stLong(MACDISK_RESULT_OFFSET);
}

// Reads count sectors at lba and compares them with what unit should hold.
// Returns the sectors that differ, or -1 with *result set when the command
// failed.
static int32_t dt_readCheck(uint8_t unit, const DtImage *img, uint32_t lba,
                            uint32_t count, int32_t *result) {
  *result = dt_command(MACDISK_CMD_READ, unit | (count << 16), lba, NULL, 0);
  if (*result != MACDISK_OK) return -1;
  int32_t bad = 0;
  for (uint32_t s = 0; s < count; s++) {
    uint32_t seed = dt_seedOf(unit, img, lba + s);
    uint32_t base = MACDISK_BUFFER_OFFSET + s * DT_SECTOR;
    for (uint32_t i = 0; i < DT_SECTOR; i++) {
      if (dt_stByte(base + i) != dt_pattern(seed, lba + s, i)) {
        bad++;
        break;
      }
    }
  }
  return bad;
}

// ---------------------------------------------------------------------------
// The images.

static void dt_be32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

static FRESULT dt_writeBytes(FIL *f, const void *bytes, UINT length) {
  UINT written = 0;
  FRESULT fr = f_write(f, bytes, length, &written);
  return (fr == FR_OK && written != length) ? FR_DISK_ERR : fr;
}

static FRESULT dt_writePattern(FIL *f, uint32_t seed, uint32_t lba,
                               uint32_t count) {
  while (count > 0) {
    uint32_t n =
        count < sizeof(chunk) / DT_SECTOR ? count : sizeof(chunk) / DT_SECTOR;
    for (uint32_t k = 0; k < n; k++) {
      dt_fillSector(chunk + k * DT_SECTOR, seed, lba + k);
    }
    FRESULT fr = dt_writeBytes(f, chunk, (UINT)(n * DT_SECTOR));
    if (fr != FR_OK) return fr;
    lba += n;
    count -= n;
  }
  return FR_OK;
}

static FRESULT dt_writeZeros(FIL *f, uint32_t length) {
  memset(chunk, 0, sizeof(chunk));
  while (length > 0) {
    UINT n = length < sizeof(chunk) ? (UINT)length : (UINT)sizeof(chunk);
    FRESULT fr = dt_writeBytes(f, chunk, n);
    if (fr != FR_OK) return fr;
    length -= n;
  }
  return FR_OK;
}

static FRESULT dt_writeHeaders(FIL *f, const DtImage *img) {
  uint8_t header[MACBINARY_HEADER_SIZE];
  uint32_t dataBytes = img->sectors * DT_SECTOR;
  if (img->format == MACDISK_FORMAT_MACBINARY_DC42) {
    memset(header, 0, sizeof(header));
    header[1] = 8;
    memcpy(header + 2, "Selftest", 8);
    memcpy(header + 65, "dImgdCpy", 8);
    dt_be32(header + 83, DC42_HEADER_SIZE + dataBytes + img->tagBytes);
    FRESULT fr = dt_writeBytes(f, header, MACBINARY_HEADER_SIZE);
    if (fr != FR_OK) return fr;
  }
  if (img->format == MACDISK_FORMAT_MACBINARY_DC42 ||
      img->format == MACDISK_FORMAT_DC42) {
    memset(header, 0, DC42_HEADER_SIZE);
    header[0] = 8;
    memcpy(header + 1, "Selftest", 8);
    dt_be32(header + 0x40, dataBytes);
    dt_be32(header + 0x44, img->tagBytes);
    header[0x50] = (img->sectors == DT_FD800_SECTORS) ? 1 : 0;
    header[0x51] = (img->sectors == DT_FD800_SECTORS) ? 0x22 : 0x12;
    header[0x52] = 0x01;
    header[0x53] = 0x00;
    return dt_writeBytes(f, header, DC42_HEADER_SIZE);
  }
  return FR_OK;
}

// Writes img into dir. With fragment, a filler file grows a cluster at a time
// between its pieces, so its clusters end up in about DT_FRAGMENTS runs.
static bool dt_createImage(const char *dir, const DtImage *img, bool fragment) {
  snprintf(path, sizeof(path), "%s/%s", dir, img->name);
  (void)f_chmod(path, 0, AM_RDO);  // a read-only one from the last run
  FRESULT fr = f_open(&file, path, FA_CREATE_ALWAYS | FA_WRITE);
  if (fr != FR_OK) {
    dt_check(false, "create %s (%d)", path, (int)fr);
    return false;
  }
  uint32_t clusterSectors = file.obj.fs->csize;
  uint32_t piece = img->sectors;
  if (fragment) {
    uint32_t clusters = img->sectors / clusterSectors;
    uint32_t pieceClusters = clusters / DT_FRAGMENTS;
    if (pieceClusters == 0) pieceClusters = 1;
    piece = pieceClusters * clusterSectors;
    hdPieceSectors = piece;
    snprintf(path, sizeof(path), "%s/FILL.BIN", dir);
    fr = f_open(&filler, path, FA_CREATE_ALWAYS | FA_WRITE);
  }
  if (fr == FR_OK) fr = dt_writeHeaders(&file, img);
  for (uint32_t lba = 0; fr == FR_OK && lba < img->sectors; lba += piece) {
    uint32_t n = img->sectors - lba < piece ? img->sectors - lba : piece;
    fr = dt_writePattern(&file, img->seed, lba, n);
    if (fr == FR_OK && fragment) {
      fr = dt_writeZeros(&filler, clusterSectors * DT_SECTOR);
    }
  }
  if (fr == FR_OK && img->tagBytes > 0)
    fr = dt_writeZeros(&file, img->tagBytes);
  FRESULT closed = f_close(&file);
  if (fr == FR_OK) fr = closed;
  if (fragment) {
    f_close(&filler);
    f_unlink(path);  // the filler: its clusters become the holes between
  }
  snprintf(path, sizeof(path), "%s/%s", dir, img->name);
  if (fr == FR_OK && img->readOnly && img->format == MACDISK_FORMAT_RAW) {
    fr = f_chmod(path, AM_RDO, AM_RDO);
  }
  if (fr != FR_OK) dt_check(false, "write %s (%d)", path, (int)fr);
  return fr == FR_OK;
}

static bool dt_createSet(const char *dir, const DtImage set[MACDISK_UNITS]) {
  FRESULT fr = f_mkdir(dir);
  if (fr != FR_OK && fr != FR_EXIST) {
    dt_check(false, "create %s (%d)", dir, (int)fr);
    return false;
  }
  bool ok = true;
  for (uint8_t unit = 0; unit < MACDISK_UNITS; unit++) {
    bool fragment = (set == dtSetA) && unit == MACDISK_UNIT_HD0;
    ok = dt_createImage(dir, &set[unit], fragment) && ok;
  }
  dt_check(ok, "%s: images created", dir);
  return ok;
}

static void dt_mountSet(const char *dir) {
  macdisk_unmountAll();
  macdisk_mountAll(dir);
  for (uint8_t unit = 0; unit < MACDISK_UNITS; unit++) writeCount[unit] = 0;
}

// ---------------------------------------------------------------------------
// The checks.

static void dt_checkStatus(const char *setName,
                           const DtImage set[MACDISK_UNITS]) {
  for (uint8_t unit = 0; unit < MACDISK_UNITS; unit++) {
    const DtImage *img = &set[unit];
    uint32_t flags = dt_unitField(unit, MACDISK_UNIT_FLAGS);
    uint32_t sectors = dt_unitField(unit, MACDISK_UNIT_SECTORS);
    uint32_t format = dt_unitField(unit, MACDISK_UNIT_FORMAT);
    uint32_t want =
        MACDISK_FLAG_INSERTED | (img->readOnly ? MACDISK_FLAG_READONLY : 0u);
    dt_check(flags == want && sectors == img->sectors && format == img->format,
             "%s %s %s: flags %lu, %lu sectors, format %lu", setName,
             dtUnitNames[unit], img->name, (unsigned long)flags,
             (unsigned long)sectors, (unsigned long)format);
  }
}

static void dt_checkFullRead(const char *setName, uint8_t unit,
                             const DtImage *img) {
  uint32_t bad = 0;
  uint32_t reads = 0;
  int32_t firstError = MACDISK_OK;
  int32_t result = MACDISK_OK;
  for (uint32_t lba = 0; lba < img->sectors; lba += MACDISK_MAX_READ_SECTORS) {
    uint32_t count = img->sectors - lba;
    if (count > MACDISK_MAX_READ_SECTORS) count = MACDISK_MAX_READ_SECTORS;
    int32_t wrong = dt_readCheck(unit, img, lba, count, &result);
    reads++;
    if (wrong < 0) {
      if (firstError == MACDISK_OK) firstError = result;
      bad += count;
    } else {
      bad += (uint32_t)wrong;
    }
  }
  // Odd lengths and the last sectors, as a driver asks for them.
  const uint32_t odd[][2] = {{img->sectors - 1, 1},
                             {100, 7},
                             {img->sectors - 31, 31},
                             {img->sectors - 2, 2}};
  for (size_t k = 0; k < sizeof(odd) / sizeof(odd[0]); k++) {
    int32_t wrong = dt_readCheck(unit, img, odd[k][0], odd[k][1], &result);
    reads++;
    if (wrong < 0) {
      if (firstError == MACDISK_OK) firstError = result;
      bad += odd[k][1];
    } else {
      bad += (uint32_t)wrong;
    }
  }
  dt_check(bad == 0 && firstError == MACDISK_OK,
           "%s %s: read all %lu sectors in %lu reads, %lu wrong, result %ld",
           setName, dtUnitNames[unit], (unsigned long)img->sectors,
           (unsigned long)reads, (unsigned long)bad, (long)firstError);
}

// The writes for unit: the first and last sectors, a cluster boundary, the
// hard disk's first fragment boundary, and DT_RANDOM_WRITES more.
static uint32_t dt_planWrites(uint8_t unit, const DtImage *img,
                              uint32_t clusterSectors, DtWrite *plan) {
  uint32_t n = 0;
  plan[n++] = (DtWrite){0, 2, 0};
  plan[n++] = (DtWrite){img->sectors - 2, 2, 0};
  plan[n++] = (DtWrite){img->sectors - 1, 1, 0};
  if (clusterSectors >= 2 && clusterSectors < img->sectors) {
    plan[n++] = (DtWrite){clusterSectors - 1, 2, 0};
  }
  if (unit == MACDISK_UNIT_HD0 && hdPieceSectors >= 2 &&
      hdPieceSectors < img->sectors) {
    plan[n++] = (DtWrite){hdPieceSectors - 1, 2, 0};
  }
  uint32_t x = 0x1234567u + unit;
  for (uint32_t k = 0; k < DT_RANDOM_WRITES && n < DT_MAX_WRITES; k++) {
    x = x * 1103515245u + 12345u;
    plan[n++] = (DtWrite){(x >> 8) % (img->sectors - 1), 1 + (x & 1), 0};
  }
  for (uint32_t k = 0; k < n; k++) {
    plan[k].seed = 0x5EED0000u | ((uint32_t)unit << 8) | k;
  }
  return n;
}

static void dt_checkWrites(uint8_t unit, const DtImage *img,
                           uint32_t clusterSectors) {
  DtWrite plan[DT_MAX_WRITES];
  uint32_t n = dt_planWrites(unit, img, clusterSectors, plan);
  uint32_t failed = 0;
  char where[48] = "";
  for (uint32_t k = 0; k < n; k++) {
    const DtWrite *w = &plan[k];
    for (uint32_t s = 0; s < w->count; s++) {
      dt_fillSector(data + s * DT_SECTOR, w->seed, w->lba + s);
    }
    writes[unit][writeCount[unit]++] = *w;
    int32_t result = MACDISK_OK;
    bool ok = true;
    // The write, then the same write again, as a retry sends it.
    for (int attempt = 0; attempt < 2 && ok; attempt++) {
      ok = dt_command(MACDISK_CMD_WRITE, unit | (w->count << 16), w->lba, data,
                      w->count * DT_SECTOR) == MACDISK_OK &&
           dt_readCheck(unit, img, w->lba, w->count, &result) == 0;
    }
    // The neighbours keep what they held.
    if (ok && w->lba > 0) {
      ok = dt_readCheck(unit, img, w->lba - 1, 1, &result) == 0;
    }
    if (ok && w->lba + w->count < img->sectors) {
      ok = dt_readCheck(unit, img, w->lba + w->count, 1, &result) == 0;
    }
    if (!ok && failed++ == 0) {
      snprintf(where, sizeof(where), ", first at %lu+%lu",
               (unsigned long)w->lba, (unsigned long)w->count);
    }
  }
  dt_check(failed == 0,
           "A %s: %lu writes, each read back, repeated and its neighbours "
           "intact%s",
           dtUnitNames[unit], (unsigned long)n, where);
}

// After an eject, which syncs, the card holds every write at the right offset.
static void dt_checkOnCard(uint8_t unit, const DtImage *img) {
  snprintf(path, sizeof(path), "%s/%s", setA, img->name);
  FRESULT fr = f_open(&file, path, FA_OPEN_EXISTING | FA_READ);
  uint32_t bad = 0;
  for (uint32_t k = 0; fr == FR_OK && k < writeCount[unit]; k++) {
    const DtWrite *w = &writes[unit][k];
    for (uint32_t s = 0; s < w->count; s++) {
      UINT got = 0;
      uint32_t lba = w->lba + s;
      fr = f_lseek(&file, img->dataOffset + (FSIZE_t)lba * DT_SECTOR);
      if (fr == FR_OK) fr = f_read(&file, chunk, DT_SECTOR, &got);
      if (fr != FR_OK || got != DT_SECTOR) {
        bad++;
        continue;
      }
      uint32_t seed = dt_seedOf(unit, img, lba);
      for (uint32_t i = 0; i < DT_SECTOR; i++) {
        if (chunk[i] != dt_pattern(seed, lba, i)) {
          bad++;
          break;
        }
      }
    }
  }
  f_close(&file);
  dt_check(fr == FR_OK && bad == 0,
           "A %s: after the eject the card holds every write (%lu wrong, %d)",
           dtUnitNames[unit], (unsigned long)bad, (int)fr);
}

static void dt_checkRefusals(void) {
  const DtImage *hd = &dtSetA[MACDISK_UNIT_HD0];
  const uint32_t hd0 = MACDISK_UNIT_HD0;
  struct {
    const char *what;
    uint16_t command;
    uint32_t d3;
    uint32_t d4;
    uint32_t length;
    int32_t want;
  } cases[] = {
      {"read 0 sectors", MACDISK_CMD_READ, hd0, 0, 0, MACDISK_ERR_PARAM},
      {"read 33 sectors", MACDISK_CMD_READ, hd0 | (33u << 16), 0, 0,
       MACDISK_ERR_PARAM},
      {"read unit 3", MACDISK_CMD_READ, 3 | (1u << 16), 0, 0,
       MACDISK_ERR_PARAM},
      {"read past the end", MACDISK_CMD_READ, hd0 | (1u << 16), hd->sectors, 0,
       MACDISK_ERR_RANGE},
      {"read across the end", MACDISK_CMD_READ, hd0 | (2u << 16),
       hd->sectors - 1, 0, MACDISK_ERR_RANGE},
      {"read at sector 2^32-1", MACDISK_CMD_READ, hd0 | (1u << 16), 0xFFFFFFFFu,
       0, MACDISK_ERR_RANGE},
      {"write 3 sectors", MACDISK_CMD_WRITE, hd0 | (3u << 16), 0, sizeof(data),
       MACDISK_ERR_PARAM},
      {"write 2 sectors of data 1", MACDISK_CMD_WRITE, hd0 | (2u << 16), 0,
       DT_SECTOR, MACDISK_ERR_PARAM},
      {"unknown command", (APP_MACDISK << 8) | 0x7F, hd0 | (1u << 16), 0, 0,
       MACDISK_ERR_PARAM},
  };
  memset(data, 0xA5, sizeof(data));
  uint32_t wrong = 0;
  char first[64] = "";
  for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
    int32_t got = dt_command(cases[k].command, cases[k].d3, cases[k].d4, data,
                             cases[k].length);
    if (got != cases[k].want && wrong++ == 0) {
      snprintf(first, sizeof(first), ", first: %s answered %ld", cases[k].what,
               (long)got);
    }
  }
  dt_check(wrong == 0, "A: %lu bad requests refused%s",
           (unsigned long)(sizeof(cases) / sizeof(cases[0])), first);
}

static void dt_checkEject(void) {
  const uint8_t unit = MACDISK_UNIT_FD1;
  uint32_t before = dt_unitField(unit, MACDISK_UNIT_CHANGES);
  int32_t first = dt_command(MACDISK_CMD_EJECT, unit, 0, NULL, 0);
  uint32_t flags = dt_unitField(unit, MACDISK_UNIT_FLAGS);
  uint32_t sectors = dt_unitField(unit, MACDISK_UNIT_SECTORS);
  uint32_t changes = dt_unitField(unit, MACDISK_UNIT_CHANGES);
  int32_t read = dt_command(MACDISK_CMD_READ, unit | (1u << 16), 0, NULL, 0);
  int32_t second = dt_command(MACDISK_CMD_EJECT, unit, 0, NULL, 0);
  uint32_t after = dt_unitField(unit, MACDISK_UNIT_CHANGES);
  dt_check(first == MACDISK_OK && second == MACDISK_OK && flags == 0 &&
               sectors == 0 && changes == before + 1 && after == changes &&
               read == MACDISK_ERR_NOMEDIUM,
           "A FD1: eject empties it, moves its counter once (%lu -> %lu), "
           "a read answers %ld",
           (unsigned long)before, (unsigned long)after, (long)read);
}

static void dt_checkPram(void) {
  snprintf(path, sizeof(path), "%s/%s", setA, MACDISK_PRAM_FILE);
  f_unlink(path);
  int32_t none = dt_command(MACDISK_CMD_PRAM_READ, 0, 0, NULL, 0);
  for (uint32_t i = 0; i < MACDISK_PRAM_SIZE; i++) {
    data[i] = dt_pattern(0x9A3A0000u, 0, i);
  }
  int32_t first =
      dt_command(MACDISK_CMD_PRAM_WRITE, 0, 0, data, MACDISK_PRAM_SIZE);
  int32_t again =
      dt_command(MACDISK_CMD_PRAM_WRITE, 0, 0, data, MACDISK_PRAM_SIZE);
  int32_t read = dt_command(MACDISK_CMD_PRAM_READ, 0, 0, NULL, 0);
  uint32_t bad = 0;
  for (uint32_t i = 0; i < MACDISK_PRAM_SIZE; i++) {
    if (dt_stByte(MACDISK_BUFFER_OFFSET + i) != data[i]) bad++;
  }
  int32_t shortWrite =
      dt_command(MACDISK_CMD_PRAM_WRITE, 0, 0, data, MACDISK_PRAM_SIZE - 2);
  FILINFO info;
  FRESULT fr = f_stat(path, &info);
  dt_check(none == MACDISK_ERR_NOMEDIUM && first == MACDISK_OK &&
               again == MACDISK_OK && read == MACDISK_OK && bad == 0 &&
               shortWrite == MACDISK_ERR_PARAM && fr == FR_OK &&
               info.fsize == MACDISK_PRAM_SIZE,
           "A PRAM: none %ld, written, repeated, read back (%lu wrong), "
           "a short write %ld, file %lu bytes",
           (long)none, (unsigned long)bad, (long)shortWrite,
           (unsigned long)(fr == FR_OK ? info.fsize : 0));
}

static void dt_checkReadOnly(uint8_t unit, const DtImage *img) {
  dt_fillSector(data, 0x0BAD0000u, 0);
  int32_t result = MACDISK_OK;
  int32_t write =
      dt_command(MACDISK_CMD_WRITE, unit | (1u << 16), 0, data, DT_SECTOR);
  int32_t wrong = dt_readCheck(unit, img, 0, 1, &result);
  dt_check(write == MACDISK_ERR_READONLY && wrong == 0,
           "B %s: a write answers %ld and changes nothing", dtUnitNames[unit],
           (long)write);
}

// ---------------------------------------------------------------------------

static void dt_run(const char *folder) {
  DevdisktestState *st = &devdisktestState;
  st->seq++;
  st->state = DEVDISKTEST_RUNNING;
  st->checks = st->failures = st->elapsedMs = 0;
  st->read32Count = st->read32MaxUs = st->read32TotalUs = 0;
  st->write2Count = st->write2MaxUs = st->write2TotalUs = 0;
  st->commandMaxUs = st->clusterBytes = 0;
  st->firstFailure[0] = '\0';
  uint32_t startMs = to_ms_since_boot(get_absolute_time());

  if (!sdcard_isMounted()) {
    dt_check(false, "no card mounted");
    st->state = DEVDISKTEST_DONE;
    return;
  }
  snprintf(path, sizeof(path), "%s/%s", folder, DEVDISKTEST_REPORT);
  reportOpen = f_open(&report, path, FA_CREATE_ALWAYS | FA_WRITE) == FR_OK;
  if (reportOpen) st->clusterBytes = report.obj.fs->csize * DT_SECTOR;
  dt_line("md-jackintosh disk self-test %s (%s), run %lu", RELEASE_VERSION,
          RELEASE_DATE, (unsigned long)st->seq);
  dt_line("folder %s, card cluster %lu bytes", folder,
          (unsigned long)st->clusterBytes);
  uint32_t clusterSectors = st->clusterBytes / DT_SECTOR;

  macdisk_unmountAll();
  snprintf(setA, sizeof(setA), "%s/%s", folder, DEVDISKTEST_FOLDER);
  FRESULT fr = f_mkdir(setA);
  bool created = (fr == FR_OK || fr == FR_EXIST);
  snprintf(setA, sizeof(setA), "%s/%s/A", folder, DEVDISKTEST_FOLDER);
  snprintf(setB, sizeof(setB), "%s/%s/B", folder, DEVDISKTEST_FOLDER);
  created = created && dt_createSet(setA, dtSetA) && dt_createSet(setB, dtSetB);

  if (created) {
    dt_line("set A: %s, writable; the hard disk in pieces of %lu sectors", setA,
            (unsigned long)hdPieceSectors);
    dt_mountSet(setA);
    dt_check(dt_stLong(MACDISK_ID_OFFSET) == MACDISK_ID, "identity 0x%08lx",
             (unsigned long)dt_stLong(MACDISK_ID_OFFSET));
    dt_checkStatus("A", dtSetA);
    for (uint8_t unit = 0; unit < MACDISK_UNITS; unit++) {
      dt_checkFullRead("A", unit, &dtSetA[unit]);
    }
    for (uint8_t unit = 0; unit < MACDISK_UNITS; unit++) {
      dt_checkWrites(unit, &dtSetA[unit], clusterSectors);
    }
    dt_checkRefusals();
    dt_checkPram();
    int32_t timeResult = dt_command(MACDISK_CMD_TIME, 0, 0, NULL, 0);
    dt_check(timeResult == MACDISK_OK, "TIME answers %ld; valid %lu",
             (long)timeResult,
             (unsigned long)dt_stLong(MACDISK_TIME_FLAGS_OFFSET));

    // The writes reach the card, and survive a mount.
    macdisk_unmountAll();
    for (uint8_t unit = 0; unit < MACDISK_UNITS; unit++) {
      dt_checkOnCard(unit, &dtSetA[unit]);
    }
    macdisk_mountAll(setA);
    for (uint8_t unit = 0; unit < MACDISK_UNITS; unit++) {
      uint32_t bad = 0;
      int32_t result = MACDISK_OK;
      for (uint32_t k = 0; k < writeCount[unit]; k++) {
        const DtWrite *w = &writes[unit][k];
        int32_t wrong =
            dt_readCheck(unit, &dtSetA[unit], w->lba, w->count, &result);
        bad += (wrong < 0) ? w->count : (uint32_t)wrong;
      }
      dt_check(bad == 0,
               "A %s: mounted again, every write reads back (%lu "
               "wrong)",
               dtUnitNames[unit], (unsigned long)bad);
    }
    dt_checkEject();

    dt_line("set B: %s, read-only", setB);
    dt_mountSet(setB);
    dt_checkStatus("B", dtSetB);
    for (uint8_t unit = 0; unit < MACDISK_UNITS; unit++) {
      dt_checkFullRead("B", unit, &dtSetB[unit]);
      dt_checkReadOnly(unit, &dtSetB[unit]);
    }
  }

  // The card's own images back in the units.
  macdisk_unmountAll();
  macdisk_mountAll(folder);
  dt_check(dt_stLong(MACDISK_ID_OFFSET) == MACDISK_ID,
           "the card's own images mounted again");

  st->elapsedMs = to_ms_since_boot(get_absolute_time()) - startMs;
  dt_line("READ of 32 sectors: %lu, longest %lu us, mean %lu us",
          (unsigned long)st->read32Count, (unsigned long)st->read32MaxUs,
          (unsigned long)(st->read32Count ? st->read32TotalUs / st->read32Count
                                          : 0));
  dt_line("WRITE of 2 sectors: %lu, longest %lu us, mean %lu us",
          (unsigned long)st->write2Count, (unsigned long)st->write2MaxUs,
          (unsigned long)(st->write2Count ? st->write2TotalUs / st->write2Count
                                          : 0));
  dt_line("any command: longest %lu us", (unsigned long)st->commandMaxUs);
  dt_line("%s: %lu checks, %lu failed, %lu ms",
          st->failures ? "FAILED" : "PASSED", (unsigned long)st->checks,
          (unsigned long)st->failures, (unsigned long)st->elapsedMs);
  if (reportOpen) {
    f_close(&report);
    reportOpen = false;
  }
  st->state = DEVDISKTEST_DONE;
}

void devdisktest_runIfRequested(const char *folder) {
  if (!sdcard_isMounted()) return;
  snprintf(path, sizeof(path), "%s/%s", folder, DEVDISKTEST_TRIGGER);
  FILINFO info;
  if (f_stat(path, &info) != FR_OK) return;
  // Gone before the run: a test that hangs or crashes the RP runs once.
  f_unlink(path);
  DPRINTF("disktest: %s found\n", path);
  dt_run(folder);
}

uint32_t devdisktest_request(void) {
  if (devdisktestState.state == DEVDISKTEST_REQUESTED ||
      devdisktestState.state == DEVDISKTEST_RUNNING) {
    return 0;
  }
  devdisktestState.state = DEVDISKTEST_REQUESTED;
  return 1;
}

void devdisktest_poll(const char *folder) {
  if (devdisktestState.state == DEVDISKTEST_REQUESTED) dt_run(folder);
}

#endif
