#!/usr/bin/env python3
"""Rod Bluetooth: one small live message, 10 times a second, for everyone.

Before, the watch asked "/watch" 5-10 times a second and the phone's
background alarm asked "/data" (1 KB) every second, and every answer went
to both devices. Now the rod sends the watch's small message (strike,
pull %, tilt, knocks, battery...) by itself, 10 times a second, between
answers. Bluetooth delivers that one message to the phone AND the watch,
so a second device costs nothing. Requests ("/data" for the open
dashboard, buttons, settings) still work as before.

The live message looks like an answer with the id "live":
    #live 200 <length>\\n{...same JSON as /watch...}
Older apps/watches skip it (not a number) and keep asking as before.

Run after add_watch_pages.py.

    python3 add_live_broadcast.py <sketch.ino>      (edits in place)
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


# ---------- BleServer: a live message between answers ----------
rep('''  void on(const char* path, Handler h)
  {''', '''  typedef String (*LiveBuilder)();

  // The live message: built and sent to everyone every LIVE_MS
  void onLive(LiveBuilder b)
  {
    liveBuilder = b;
  }

  void on(const char* path, Handler h)
  {''')

rep('''    char line[REQ_MAX];

    if(!popLine(line))
      return;

    dispatch(line);''', '''    // between answers: the live message, 10 times a second
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

    dispatch(line);''')

rep('''  static const unsigned long PIECE_GAP_US = 4000;''', '''  static const unsigned long PIECE_GAP_US = 4000;
  static const unsigned long LIVE_MS = 100;  // live message 10 times a second''')

rep('''  unsigned long lastPiece = 0;''', '''  unsigned long lastPiece = 0;
  unsigned long lastLive = 0;
  LiveBuilder liveBuilder = nullptr;''')

# ---------- /watch JSON reused for the live message ----------
rep('''// Only what the watch shows (much smaller than /data)
void handleWatch()
{
  String json;''', '''// Only what the watch shows (much smaller than /data). Also the live
// message the rod sends to everyone 10 times a second.
String watchJson()
{
  String json;''')

rep('''  addField(json, "batleft", batLeftMin);

  json += "}";

  server.send(200, "application/json", json);
}''', '''  addField(json, "batleft", batLeftMin);

  json += "}";

  return json;
}

void handleWatch()
{
  server.send(200, "application/json", watchJson());
}''')

rep('''  server.on("/watch", handleWatch);''', '''  server.on("/watch", handleWatch);
#if defined(ESP32)
  server.onLive(watchJson);
#endif''')

open(path, 'w').write(s)
print("ok", path)
