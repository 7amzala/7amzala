// =====================================================
// StrikeSense LoRa protocol (rod <-> watch)
//
// The SAME file must be in the rod sketch folder and in the
// watch sketch folder. Works from C and C++.
// =====================================================

#ifndef STRIKESENSE_PROTOCOL_H
#define STRIKESENSE_PROTOCOL_H

#include <stdint.h>

// ---------- Radio settings (Spain / EU) ----------
//
// 869.525 MHz is the EU 869.4-869.65 MHz band: up to 500 mW and
// 10% transmit time. 250 kHz wide (the whole band): a heartbeat takes
// ~40 ms on air, so 2 a second while something happens is ~8%,
// 1 a second when calm ~4%.

#define SS_FREQ_MHZ     869.525
#define SS_BW_KHZ       250.0
#define SS_SF           7
#define SS_CR           5        // coding rate 4/5
#define SS_SYNC_WORD    0x12
#define SS_POWER_DBM    14
#define SS_PREAMBLE     8

// Change this number on BOTH sides if a friend uses the same
// system nearby, so the watches only hear their own rod.
#define SS_NET_ID       0xA7

// ---------- Timing ----------

#define SS_HEARTBEAT_MS     1000   // rod -> watch "I'm alive" + status (calm)
#define SS_HEARTBEAT_FAST_MS 500   // the same while something happens (knock, pull, strike)
#define SS_WATCH_HB_MS      5000   // watch -> rod "I'm here" + link quality
#define SS_RESEND_MS        1000   // repeat strikes / commands until confirmed
#define SS_MAX_TRIES        15

// ---------- Packet types ----------

enum
{
  PKT_HEARTBEAT = 1,   // rod -> watch, every second
  PKT_STRIKE    = 2,   // rod -> watch, repeated until ACK
  PKT_COMMAND   = 3,   // watch -> rod, repeated until ACK
  PKT_ACK       = 4,   // both ways
  PKT_WATCH_HB  = 5    // watch -> rod, every 5 s
};

// ---------- Rod states ----------

enum
{
  ROD_LEARNING   = 0,
  ROD_PULLTEST   = 1,
  ROD_MONITORING = 2,
  ROD_SETTLING   = 3,
  ROD_PAUSED     = 4
};

// ---------- Strike types ----------

enum
{
  STRIKE_NONE     = 0,
  STRIKE_SHARP    = 1,
  STRIKE_SLOW     = 2,
  STRIKE_REPEATED = 3,
  STRIKE_DROPBACK = 4,   // DROP-BACK + PULL (a drop-back alone is not an alarm)
  STRIKE_SHAKE    = 5    // SHAKING FISH
};

// ---------- Commands (watch -> rod) ----------

enum
{
  CMD_LEARN    = 1,   // learn the environment, then pause for casting
  CMD_WATCHING = 2,   // stop the alarm, keep detecting
  CMD_REELIN   = 3,   // pause detection
  CMD_READY    = 4,   // cast done: settle + pull test, then armed
  CMD_RESUME   = 5,   // after adjusting: settle, then armed (no pull test)
  CMD_SKIPPULL = 6,   // skip the pull test
  CMD_TEACHDIR = 7,   // new pull direction: one pull test now
  CMD_RELEARN  = 8,   // learn the sea again, stay armed (no pause)
  CMD_SETTIME  = 9    // the watch's clock to a rod that lost its time (arg, tz15)
};

// heartbeat flags
#define SS_F_DROPWATCH  0x01   // drop-back seen, watching for a pull
#define SS_F_DIR        0x02   // pull direction learned

// ---------- Packets ----------

typedef struct __attribute__((packed))
{
  uint8_t net;
  uint8_t type;
  uint8_t seq;
} ss_header_t;

typedef struct __attribute__((packed))
{
  ss_header_t h;
  uint8_t  state;        // ROD_xxx
  uint8_t  strike;       // STRIKE_xxx while an alarm is active
  uint16_t strikeId;     // changes with every new strike
  uint8_t  sea;          // 0 calm, 1 moderate, 2 rough
  uint8_t  battery;      // %, 255 = not measured
  uint8_t  learnLeft;    // seconds of learning left
  int16_t  tilt10;       // tilt x10 degrees (+ toward water)
  uint8_t  bucket;       // slow pull bucket 0..100 %
  uint8_t  knocks;       // knocks in the last seconds

  // ---- rod 2.3+: what the watch screens need when Bluetooth is out of
  // range. Added at the end, so an older watch still reads the above. ----
  int16_t  ftilt10;      // tilt with less smoothing x10 (the ring follows the tip)
  uint16_t allow100;     // pull allowance x100 degrees
  uint8_t  drop;         // drop-back bucket 0..100 %
  uint8_t  flags;        // SS_F_xxx
  uint8_t  knockId;      // low byte of the rod's knock number (changes with each knock)
  uint8_t  kpeak10;      // highest knock since the last heartbeat, x10 of the knock limit
  uint8_t  waveWind;     // wave 0..3 (low 4 bits), wind 0..3 (high 4 bits)
  uint16_t batLeft;      // rod battery minutes left, 0xFFFF = still measuring
  uint16_t ptdeg100;     // pull test: how far the tip moved, x100 degrees
  uint16_t ptneed100;    // pull test: how far it must move, x100 degrees
  uint32_t utc;          // rod time, UTC seconds since 1970, 0 = unknown
  int8_t   tz15;         // local time = UTC + tz15 x 15 minutes
} ss_heartbeat_t;

#define SS_HEARTBEAT_V1_LEN 14   // size before the rod 2.3 fields

typedef struct __attribute__((packed))
{
  ss_header_t h;
  uint8_t  strike;       // STRIKE_xxx
  uint16_t strikeId;
  int16_t  peakTilt10;
} ss_strike_t;

typedef struct __attribute__((packed))
{
  ss_header_t h;
  uint8_t cmd;           // CMD_xxx
  uint32_t arg;          // CMD_SETTIME: UTC seconds (else 0)
  int8_t   tz15;         // CMD_SETTIME: time zone in 15 minute steps
} ss_command_t;

#define SS_COMMAND_V1_LEN 4      // an older watch sends only the command

typedef struct __attribute__((packed))
{
  ss_header_t h;
  int8_t   rodRssi;      // how strong the watch hears the rod (dBm)
  int8_t   rodSnr;       // dB
  uint8_t  battery;      // watch battery %, 255 = not measured
} ss_watch_hb_t;

typedef struct __attribute__((packed))
{
  ss_header_t h;
  uint8_t  ackType;      // PKT_STRIKE or PKT_COMMAND
  uint16_t ackValue;     // strikeId, or the command's seq
} ss_ack_t;

#endif
