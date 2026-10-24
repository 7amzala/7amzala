// =====================================================
// STRIKES ON THE SD CARD
//
// The board's micro-SD slot, used in SD mode (1 bit): CLK 41, CMD 39,
// D0 40, D3 38 held high. So the SPI bus stays free for the LoRa module.
// Each strike is one small file in /strikes (about 10 KB at most),
// named S00001_20251014-2142.ssk - number, then local date and time.
// At start the newest 20 are loaded, so the history is there after a
// restart. Without a card the watch keeps them in memory only.
// =====================================================

#include <Arduino.h>
#include <SD_MMC.h>
#include "strike_sd.h"
#include "strike_log.h"

#define SD_D3   38      // the slot's "CS" pin: must stay high in SD mode
#define SD_CMD  39      // the slot's "MOSI"
#define SD_D0   40      // the slot's "MISO"
#define SD_CLK  41      // the slot's "SCK"

#define SD SD_MMC

#define FILE_MAGIC 0x53534B31              // "SSK1"

static bool present = false;
static unsigned nextNumber = 1;

struct FileHead
{
  uint32_t magic;
  uint32_t utc;
  int32_t tz;
  int32_t strikeid;
  char type[24];
  float allow;
  uint16_t n;
  uint16_t pre;
};

static unsigned numberOf(const char *name)
{
  // "S00012_..." -> 12
  const char *p = strrchr(name, '/');
  p = p ? p + 1 : name;
  return p[0] == 'S' ? (unsigned)atoi(p + 1) : 0;
}

static bool load(const char *path)
{
  File f = SD.open(path, FILE_READ);

  if(!f)
    return false;

  FileHead h;
  bool ok = f.read((uint8_t *)&h, sizeof(h)) == sizeof(h) && h.magic == FILE_MAGIC && h.n > 0 && h.n <= SLOG_MAX;

  slog_t *r = ok ? slog_add_loaded(h.n) : NULL;

  if(r)
  {
    r->utc = h.utc;
    r->tz = h.tz;
    r->got_ms = 0;
    r->strikeid = h.strikeid;
    memcpy(r->type, h.type, sizeof(r->type));
    r->type[sizeof(r->type) - 1] = 0;
    r->allow = h.allow;
    r->n = h.n;
    r->pre = h.pre;
    ok = f.read((uint8_t *)r->k, h.n * 2) == h.n * 2 &&
         f.read((uint8_t *)r->t, h.n * 2) == h.n * 2 &&
         f.read(r->st, h.n) == h.n;

    if(!ok)
      r->n = 0;
  }

  f.close();
  return ok;
}

bool strike_sd_begin(void)
{
  pinMode(SD_D3, OUTPUT);
  digitalWrite(SD_D3, HIGH);

  SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0);
  present = SD_MMC.begin("/sdcard", true);         // true = 1-bit mode

  if(!present)
  {
    Serial.println("[SD] no card - strikes kept in memory only");
    return false;
  }

  if(!SD.exists("/strikes"))
    SD.mkdir("/strikes");

  // the newest 20 by number, loaded oldest first
  unsigned nums[SLOG_KEEP] = { 0 };
  String names[SLOG_KEEP];
  int found = 0;

  File dir = SD.open("/strikes");

  for(File e = dir.openNextFile(); e; e = dir.openNextFile())
  {
    unsigned n = numberOf(e.name());

    if(n >= nextNumber)
      nextNumber = n + 1;

    if(n == 0)
      continue;

    // keep the 20 biggest numbers, sorted small -> big
    int at = found < SLOG_KEEP ? found : -1;

    if(at < 0)
    {
      if(n <= nums[0])
        continue;

      for(int i = 1; i < SLOG_KEEP; i++) { nums[i - 1] = nums[i]; names[i - 1] = names[i]; }
      at = SLOG_KEEP - 1;
    }
    else
    {
      found++;
    }

    while(at > 0 && nums[at - 1] > n) { nums[at] = nums[at - 1]; names[at] = names[at - 1]; at--; }
    nums[at] = n;
    names[at] = String("/strikes/") + e.name();
  }

  dir.close();

  int loaded = 0;

  for(int i = 0; i < found; i++)
    loaded += load(names[i].c_str()) ? 1 : 0;

  Serial.printf("[SD] card found, %d strikes loaded (%s)\n", loaded, slog_memory_note());
  return true;
}

void strike_sd_save(void)
{
  if(!present)
    return;

  slog_t *r = slog_unsaved();

  if(r == NULL)
    return;

  char path[64];

  if(r->utc)
  {
    time_t local = (time_t)r->utc + r->tz * 60;
    struct tm tm;
    gmtime_r(&local, &tm);
    snprintf(path, sizeof(path), "/strikes/S%05u_%04d%02d%02d-%02d%02d.ssk", nextNumber,
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
  }
  else
  {
    snprintf(path, sizeof(path), "/strikes/S%05u.ssk", nextNumber);
  }

  File f = SD.open(path, FILE_WRITE);

  if(!f)
  {
    Serial.printf("[SD] cannot write %s\n", path);
    present = false;                       // card gone: stop trying
    return;
  }

  FileHead h = { FILE_MAGIC, r->utc, r->tz, (int32_t)r->strikeid, { 0 }, r->allow, r->n, r->pre };
  memcpy(h.type, r->type, sizeof(h.type));

  f.write((const uint8_t *)&h, sizeof(h));
  f.write((const uint8_t *)r->k, r->n * 2);
  f.write((const uint8_t *)r->t, r->n * 2);
  f.write(r->st, r->n);
  f.close();

  r->saved = true;
  nextNumber++;

  Serial.printf("[SD] saved %s (%u points; %s)\n", path, r->n, slog_memory_note());
}

bool strike_sd_present(void)
{
  return present;
}
