// =====================================================
// STRIKE LOG: the watch records each strike itself (see strike_log.h)
//
// The strike being recorded goes into one working buffer (room for the
// 3 minutes). When it is over it is copied into a block of its real size
// (a typical strike: 2-4 KB). Without the big PSRAM switched on there is
// little memory: then the oldest strikes are dropped from memory (they
// stay on the SD card) so a new one always fits.
// =====================================================

#include "strike_log.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef ESP_PLATFORM
  #include "esp_heap_caps.h"
#endif

#define QUIET_END   100                    // quiet this many points (10 s) = the strike is over

static slog_t *recs[SLOG_KEEP];            // 0 = newest
static int count = 0;
static slog_t *rec = NULL;                 // recording now (its arrays are the working buffer)
static int dropped = 0;                    // dropped from memory for lack of room

// the working buffer for the strike being recorded
static int16_t *work_k = NULL, *work_t = NULL;
static uint8_t *work_st = NULL;

// the last 10 s, always (so a recording starts 10 s before the strike)
static int16_t ring_k[SLOG_PRE], ring_t[SLOG_PRE];
static uint8_t ring_st[SLOG_PRE];
static int ring_head = 0, ring_n = 0;

static long last_id = -1;
static int quiet_run = 0;

int slog_test_limit = 0;                   // tests: pretend memory is this small (bytes, 0 = no limit)
static int used_bytes = 0;

static void *mem(size_t size)
{
  if(slog_test_limit && used_bytes + (int)size > slog_test_limit)
    return NULL;

  void *p = NULL;

#ifdef ESP_PLATFORM
  p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);             // the big 8 MB memory, if switched on
#endif

  if(p == NULL)
    p = malloc(size);

  if(p)
    used_bytes += (int)size;

  return p;
}

static void mem_free(void *p, size_t size)
{
  if(p)
  {
    free(p);
    used_bytes -= (int)size;
  }
}

static size_t data_size(int n)
{
  return (size_t)n * (2 + 2 + 1);
}

static void free_rec(slog_t *r)
{
  if(r == NULL)
    return;

  if(r->k != work_k)
    mem_free(r->k, data_size(r->n));        // (k, t, st are one block)

  mem_free(r, sizeof(slog_t));
}

static void drop_oldest(void)
{
  if(count == 0)
    return;

  slog_t *r = recs[count - 1];

  if(r == rec)
    return;                                 // never the one being recorded

  free_rec(r);
  count--;
  dropped++;
}

// a block of n points (k, t, st together); drops old strikes if needed
static bool alloc_data(slog_t *r, int n)
{
  for(;;)
  {
    uint8_t *b = (uint8_t *)mem(data_size(n));

    if(b)
    {
      r->k = (int16_t *)b;
      r->t = (int16_t *)(b + n * 2);
      r->st = b + n * 4;
      return true;
    }

    if(count <= 1)
      return false;

    drop_oldest();
  }
}

static slog_t *new_rec(void)
{
  if(count == SLOG_KEEP)
    drop_oldest();

  slog_t *r;

  for(;;)
  {
    r = (slog_t *)mem(sizeof(slog_t));

    if(r || count == 0)
      break;

    drop_oldest();
  }

  if(r == NULL)
    return NULL;

  memset(r, 0, sizeof(slog_t));
  memmove(&recs[1], &recs[0], sizeof(recs[0]) * count);
  recs[0] = r;
  count++;
  return r;
}

static void remove_rec(slog_t *r)
{
  for(int i = 0; i < count; i++)
  {
    if(recs[i] == r)
    {
      memmove(&recs[i], &recs[i + 1], sizeof(recs[0]) * (count - i - 1));
      count--;
      return;
    }
  }
}

// the recording is over: keep it in a block of its real size
static void finish(void)
{
  slog_t *r = rec;
  rec = NULL;

  if(r == NULL)
    return;

  r->done = true;

  slog_t tmp = *r;

  if(!alloc_data(r, r->n))
  {
    remove_rec(r);                          // no memory at all: not kept
    free_rec(r);
    return;
  }

  memcpy(r->k, tmp.k, r->n * 2);
  memcpy(r->t, tmp.t, r->n * 2);
  memcpy(r->st, tmp.st, r->n);
}

void slog_tick(int k, int tilt100, int fish, bool strike, long strikeid, const char *type,
               float allow, bool answered, uint32_t now_ms, uint32_t utc, int tz)
{
  k = k < 0 ? 0 : (k > 300 ? 300 : k);
  tilt100 = tilt100 < -900 ? -900 : (tilt100 > 900 ? 900 : tilt100);

  // a new strike while one is still recorded: that one is over
  if(strike && strikeid != last_id && rec != NULL)
    finish();

  // a new strike: start with the last 10 s
  if(strike && strikeid != last_id && rec == NULL)
  {
    if(work_k == NULL)
    {
      work_k = (int16_t *)mem(SLOG_MAX * 2);
      work_t = (int16_t *)mem(SLOG_MAX * 2);
      work_st = (uint8_t *)mem(SLOG_MAX);
    }

    rec = work_k && work_t && work_st ? new_rec() : NULL;

    if(rec)
    {
      rec->k = work_k;
      rec->t = work_t;
      rec->st = work_st;
      rec->utc = utc;
      rec->tz = tz;
      rec->got_ms = now_ms;
      rec->strikeid = strikeid;
      strncpy(rec->type, type ? type : "", sizeof(rec->type) - 1);
      rec->allow = allow;

      int start = (ring_head - ring_n + SLOG_PRE) % SLOG_PRE;

      for(int i = 0; i < ring_n; i++)
      {
        int j = (start + i) % SLOG_PRE;
        rec->k[i] = ring_k[j];
        rec->t[i] = ring_t[j];
        rec->st[i] = ring_st[j];
      }

      rec->n = (uint16_t)ring_n;
      rec->pre = (uint16_t)ring_n;
      quiet_run = 0;
    }
  }

  if(strike)
    last_id = strikeid;

  // keep the last 10 s
  ring_k[ring_head] = (int16_t)k;
  ring_t[ring_head] = (int16_t)tilt100;
  ring_st[ring_head] = (uint8_t)fish;
  ring_head = (ring_head + 1) % SLOG_PRE;

  if(ring_n < SLOG_PRE)
    ring_n++;

  if(rec == NULL)
    return;

  rec->k[rec->n] = (int16_t)k;
  rec->t[rec->n] = (int16_t)tilt100;
  rec->st[rec->n] = (uint8_t)fish;
  rec->n++;

  quiet_run = fish == 0 ? quiet_run + 1 : 0;

  // over: at least 10 s after the strike, and the fish quiet for 10 s
  // (or answered and quiet 3 s), or 3 minutes full
  int after = rec->n - rec->pre;
  bool quiet = quiet_run >= QUIET_END || (answered && quiet_run >= 30);

  if((after >= 100 && quiet) || rec->n >= SLOG_MAX)
    finish();
}

int slog_count(void)
{
  return count;
}

slog_t *slog_get(int i)
{
  return i >= 0 && i < count ? recs[i] : NULL;
}

bool slog_recording(void)
{
  return rec != NULL;
}

slog_t *slog_unsaved(void)
{
  for(int i = count - 1; i >= 0; i--)
  {
    if(recs[i]->done && !recs[i]->saved)
      return recs[i];
  }

  return NULL;
}

// at start, from the card: oldest first, each goes to the front
slog_t *slog_add_loaded(int n)
{
  slog_t *r = new_rec();

  if(r == NULL)
    return NULL;

  r->n = (uint16_t)n;

  if(!alloc_data(r, n))
  {
    remove_rec(r);
    free_rec(r);
    return NULL;
  }

  r->done = true;
  r->saved = true;
  r->from_card = true;
  return r;
}

const char *slog_memory_note(void)
{
  static char s[96];
  snprintf(s, sizeof(s), "%d strikes in memory, %d bytes, %d dropped for room", count, used_bytes, dropped);
  return s;
}
