#!/usr/bin/env python3
"""Rod Bluetooth: with the phone AND the watch connected, send pieces no
faster than the watch's calm (power-saving) link takes them. Every piece
goes to both; the Bluetooth chip holds only ~12 for the slow link and
drops the rest without a word, so long answers (phone LAST STRIKE, the
watch's /laststrike) arrived broken. A burst of 8 pieces, then about 4
every 110 ms: small answers stay instant, long ones take a bit longer
but arrive whole.

    python3 add_ble_pacing.py <sketch.ino>      (edits in place)
"""
import sys

path = sys.argv[1]
s = open(path).read()


def rep(old, new, count=1):
    global s
    n = s.count(old)
    if n != count:
        sys.exit(f"{path}: expected {count} x {old[:70]!r}, found {n}")
    s = s.replace(old, new)


rep('''    if(outPos < out.length())
    {
      if(clients > 0 && micros() - lastPiece >= PIECE_GAP_US)
      {''', '''    if(outPos < out.length())
    {
      // phone + watch: no faster than the watch's slow link takes them
      bool room = true;

      if(clients >= 2)
      {
        unsigned long nowMs = millis();
        pieceTokens += (nowMs - tokensAt) * PIECES_PER_110MS / 110.0f;
        tokensAt = nowMs;

        if(pieceTokens > PIECE_BURST)
          pieceTokens = PIECE_BURST;

        room = pieceTokens >= 1;
      }
      else
      {
        pieceTokens = PIECE_BURST;
        tokensAt = millis();
      }

      if(clients > 0 && room && micros() - lastPiece >= PIECE_GAP_US)
      {
        if(clients >= 2)
          pieceTokens -= 1;
''')

rep('''  static const unsigned long PIECE_GAP_US = 4000;''', '''  static const unsigned long PIECE_GAP_US = 4000;
  static constexpr float PIECE_BURST = 8;          // phone + watch: pieces at once...
  static constexpr float PIECES_PER_110MS = 4;     // ...then this many per calm watch link interval
  float pieceTokens = 8;
  unsigned long tokensAt = 0;''')

# questions first, then the live message (a waiting question must never
# starve behind live messages - with paced pieces it would)
rep('''    // between answers: the live message, 10 times a second
    if(clients > 0 && liveBuilder != nullptr && millis() - lastLive >= LIVE_MS)
    {
      lastLive = millis();
      String body = liveBuilder();
      out = "#live 200 " + String((unsigned long)body.length()) + "\\n" + body;
      outPos = 0;
      return;
    }

    char line[REQ_MAX];

    if(!popLine(line))
      return;

    dispatch(line);
  }''', '''    // between answers: a waiting question first...
    char line[REQ_MAX];

    if(popLine(line))
    {
      dispatch(line);
      return;
    }

    // ...then the live message, 10 times a second
    if(clients > 0 && liveBuilder != nullptr && millis() - lastLive >= LIVE_MS)
    {
      lastLive = millis();
      String body = liveBuilder();
      out = "#live 200 " + String((unsigned long)body.length()) + "\\n" + body;
      outPos = 0;
    }
  }''')

open(path, 'w').write(s)
print("ok", path)
