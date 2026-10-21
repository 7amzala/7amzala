#!/usr/bin/env python3
"""Rod: 5 named settings setups ("presets"), kept in flash on the rod.
Default stays fixed (/defaultsettings).

  /presets                 -> {"presets":["Calm beach","",...]}   ("" = empty)
  /presetsave?s=0&n=Name   -> keeps the current settings in setup s under that name
  /presetload?s=0          -> loads setup s into the live settings (SAVE keeps them)
  /presetdel?s=0           -> empties setup s

    python3 add_presets.py <sketch.ino>      (edits in place)
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


rep('''void handleDefaultSettings()
{''', '''// ---------- Saved setups (5, named, in flash) ----------

const int PRESET_COUNT = 5;
const int PRESET_NAME_MAX = 24;          // bytes

String presetKey(int slot, const char* key)
{
  return String("s") + String(slot) + key;   // e.g. "s0pdeg" (flash keys: max 15 letters)
}

// A name safe to put in JSON: no quotes, backslashes or control
// characters; letters like n with tilde (UTF-8) are kept; cut whole.
String cleanPresetName(const String& in)
{
  String out;

  for(unsigned i = 0; i < in.length(); i++)
  {
    unsigned char c = (unsigned char)in[i];

    if(c == '"' || c == '\\\\' || c < 0x20 || c == 0x7F)
      continue;

    out += (char)c;
  }

  out.trim();

  while(out.length() > PRESET_NAME_MAX)
  {
    int cut = PRESET_NAME_MAX;

    while(cut > 0 && (((unsigned char)out[cut]) & 0xC0) == 0x80)
      cut--;                             // not in the middle of a letter

    out = out.substring(0, cut);
  }

  return out;
}

void sendPresets()
{
  String json = "{\\"presets\\":[";

  prefs.begin("presets", true);

  for(int p = 0; p < PRESET_COUNT; p++)
  {
    if(p > 0) json += ",";

    json += "\\"";
    json += prefs.getString(presetKey(p, "name").c_str(), "");
    json += "\\"";
  }

  prefs.end();

  json += "]}";

  server.send(200, "application/json", json);
}

int presetSlot()
{
  int p = server.arg("s").toInt();
  return p >= 0 && p < PRESET_COUNT ? p : -1;
}

void handlePresets()
{
  sendPresets();
}

void handlePresetSave()
{
  int p = presetSlot();

  if(p < 0)
  {
    server.send(400, "text/plain", "BAD SETUP");
    return;
  }

  String name = cleanPresetName(server.arg("n"));

  if(name.length() == 0)
    name = String("Setup ") + String(p + 1);

  prefs.begin("presets", false);

  for(int i = 0; i < SETTING_COUNT; i++)
    prefs.putFloat(presetKey(p, SETTING_INFO[i].key).c_str(), settings.*(SETTING_INFO[i].field));

  prefs.putString(presetKey(p, "name").c_str(), name);
  prefs.end();

  Serial.printf("SETUP %d SAVED: %s\\n", p + 1, name.c_str());

  sendPresets();
}

void handlePresetLoad()
{
  int p = presetSlot();

  prefs.begin("presets", true);
  bool used = p >= 0 && prefs.getString(presetKey(p, "name").c_str(), "").length() > 0;

  if(used)
  {
    for(int i = 0; i < SETTING_COUNT; i++)
    {
      const SettingInfo& info = SETTING_INFO[i];

      settings.*(info.field) =
        clampFloat(
          prefs.getFloat(presetKey(p, info.key).c_str(), DEFAULT_SETTINGS.*(info.field)),
          info.low,
          info.high
        );
    }
  }

  prefs.end();

  if(!used)
  {
    server.send(404, "text/plain", "EMPTY SETUP");
    return;
  }

  updateKnockThreshold();

  sendSettings();
}

void handlePresetDelete()
{
  int p = presetSlot();

  if(p >= 0)
  {
    prefs.begin("presets", false);

    for(int i = 0; i < SETTING_COUNT; i++)
      prefs.remove(presetKey(p, SETTING_INFO[i].key).c_str());

    prefs.remove(presetKey(p, "name").c_str());
    prefs.end();
  }

  sendPresets();
}

void handleDefaultSettings()
{''')

rep('''  server.on("/defaultsettings", handleDefaultSettings);''', '''  server.on("/defaultsettings", handleDefaultSettings);
  server.on("/presets", handlePresets);
  server.on("/presetsave", handlePresetSave);
  server.on("/presetload", handlePresetLoad);
  server.on("/presetdel", handlePresetDelete);''')

open(path, 'w').write(s)
print("ok", path)
