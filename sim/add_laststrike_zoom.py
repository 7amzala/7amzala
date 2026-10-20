#!/usr/bin/env python3
"""Watch LAST STRIKE: shown at once, plus a 2 s + 2 s ZOOM of the strike.

- While a strike is still being recorded (the 10 s after it), "/laststrike"
  sends what is there so far (20 s before + the seconds after so far), so
  the watch draws it at once instead of an empty page.
- "zk"/"zt": 2 s before + 2 s after the strike at 50 points a second (from
  the dashboard's strike recording), to see how the fish hit.

Run after add_watch_pages.py.

    python3 add_laststrike_zoom.py <sketch.ino>      (edits in place)
"""
import sys

path = sys.argv[1]
s = open(path).read()

start = s.index("// The watch's LAST STRIKE page (fresh after a rod restart)\nvoid handleLastStrike()")
end = s.index("\n}\n", start) + 3

s = s[:start] + '''// The watch's LAST STRIKE page (fresh after a rod restart).
// Recording (the 10 s after a strike): what is there so far, so the watch
// shows it at once. Plus "zk"/"zt": 2 s before + 2 s after at 50 a second.
const int ZOOM_EACH = 100;          // 2 s at 50 a second, before and after

void handleLastStrike()
{
  String json;
  json.reserve(WREC_N * 10 + ZOOM_EACH * 20 + 300);

  bool live = wsRecording;
  int n = live ? wrecCount : wsCount;
  int start = live ? (wrecHead - wrecCount + WREC_N) % WREC_N : 0;
  bool have = live || wsHave;

  json += "{\\"type\\":\\"";
  json += live ? wsNextType : wsType;
  json += "\\"";

  addField(json, "have", have);
  addField(json, "recording", live);
  addField(json, "ago", have ? (long)((millis() - (live ? wsNextAt : wsAt)) / 1000) : 0L);
  addField(json, "count", (long)strikeEventID);
  addField(json, "tiltpeak", live ? wsNextTiltPeak : wsTiltPeak, 2);
  addField(json, "pull", (long)(live ? wsNextPull : wsPull));
  addField(json, "pre", (long)(live ? n - wsPost : n - WREC_POST));

  json += ",\\"k\\":[";

  for(int i = 0; i < n; i++)
  {
    if(i > 0) json += ",";
    json += String((int)(live ? wrecK[(start + i) % WREC_N] : wsK[i]));
  }

  json += "],\\"t\\":[";

  for(int i = 0; i < n; i++)
  {
    if(i > 0) json += ",";
    json += String((int)(live ? wrecT[(start + i) % WREC_N] : wsT[i]));
  }

  // ZOOM: the dashboard's recording of the same strike (50 a second)
  int zs = savedPre - ZOOM_EACH;
  int ze = savedPre + ZOOM_EACH;

  if(zs < 0) zs = 0;
  if(ze > savedCount) ze = savedCount;
  if(!have) ze = zs;

  float thr = savedThreshold > 0 ? savedThreshold : 1;

  json += "],\\"zpre\\":";
  json += String(savedPre - zs);
  json += ",\\"zk\\":[";

  for(int i = zs; i < ze; i++)
  {
    float k = savedEnergy[i] / thr * 100;
    if(i > zs) json += ",";
    json += String((int)(k > 300 ? 300 : k));
  }

  json += "],\\"zt\\":[";

  for(int i = zs; i < ze; i++)
  {
    float t = savedTilt[i] * 100;
    if(i > zs) json += ",";
    json += String((int)(t > 300 ? 300 : (t < -300 ? -300 : t)));
  }

  json += "]}";

  server.send(200, "application/json", json);
}
''' + s[end:]

open(path, 'w').write(s)
print("ok", path)
