// =====================================================
// STRIKE LOG: the watch records each strike itself (see strike_log.h)
// =====================================================

#include "strike_log.h"
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
  #include "esp_heap_caps.h"
#endif

#define QUIET_END   100                    // quiet this many points (10 s) = the strike is over

static slog_t *recs[SLOG_KEEP];            // 0 = newest
static int count = 0;
static slog_t *rec = NULL;                 // recording now

// the last 10 s, always (so a recording starts 10 s before the strike)
static int16_t ring_k[SLOG_PRE], ring_t[SLOG_PRE];
static uint8_t ring_st[SLOG_PRE];
static int ring_head = 0, ring_n = 0;

static long last_id = -1;
static int quiet_run = 0;

static slog_t *alloc_rec(void)
{
  slog_t *r = NULL;

#ifdef ESP_PLATFORM
  r = (slog_t *)heap_caps_malloc(sizeof(slog_t), MALLOC_CAP_SPIRAM);   // the big 8 MB memory
#endif

  if(r == NULL)
    r = (slog_t *)malloc(sizeof(slog_t));

  if(r)
    memset(r, 0, sizeof(slog_t));

  return r;
}

// a new one at the front; the oldest drops out (it is on the card)
static slog_t *push_front(void)
{
  slog_t *r;

  if(count == SLOG_KEEP)
  {
    r = recs[SLOG_KEEP - 1];
    memset(r, 0, sizeof(slog_t));
    count--;
  }
  else
  {
    r = alloc_rec();

    if(r == NULL)
      return NULL;
  }

  memmove(&recs[1], &recs[0], sizeof(recs[0]) * count);
  recs[0] = r;
  count++;
  return r;
}

void slog_tick(int k, int tilt100, int fish, bool strike, long strikeid, const char *type,
               float allow, bool answered, uint32_t now_ms, uint32_t utc, int tz)
{
  k = k < 0 ? 0 : (k > 300 ? 300 : k);
  tilt100 = tilt100 < -900 ? -900 : (tilt100 > 900 ? 900 : tilt100);

  // a new strike while one is still recorded: that one is over
  if(strike && strikeid != last_id && rec != NULL)
  {
    rec->done = true;
    rec = NULL;
  }

  // a new strike: start with the last 10 s
  if(strike && strikeid != last_id && rec == NULL)
  {
    rec = push_front();

    if(rec)
    {
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
  // (or answered and quiet), or 3 minutes full
  int after = rec->n - rec->pre;
  bool quiet = quiet_run >= QUIET_END || (answered && quiet_run >= 30);

  if((after >= 100 && quiet) || rec->n >= SLOG_MAX)
  {
    rec->done = true;
    rec = NULL;
  }
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
slog_t *slog_add_loaded(void)
{
  slog_t *r = push_front();

  if(r)
  {
    r->done = true;
    r->saved = true;
    r->from_card = true;
  }

  return r;
}
