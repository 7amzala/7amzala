#!/usr/bin/env python3
"""Dashboard LAST STRIKE: a failed load says so (not "No strike recorded
yet") and is tried again at most every 5 s (not with every data update).

    python3 add_replay_retry.py <sketch.ino>      (edits in place)
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


rep('''async function loadReplay(id)
{
  try
  {
    const response = await fetch("/strike");
    replay = await response.json();
    replayID = id;
  }
  catch(e)
  {
  }
}''', '''let replayTryAt = 0;
let replayFailed = false;

async function loadReplay(id)
{
  replayTryAt = Date.now();

  try
  {
    const response = await fetch("/strike");

    if(!response.ok)
      throw new Error("rod answered " + response.status);

    replay = await response.json();
    replayID = id;
    replayFailed = false;
  }
  catch(e)
  {
    replayFailed = true;
  }
}''')

rep('''    if(replayID !== lastStrikeID)
      await loadReplay(lastStrikeID);

    el("graphTitle").innerText = "LAST STRIKE REPLAY";
    el("graphSub").innerText = replay ? replay.type + " · 4 s before and after" : "No strike recorded yet";''', '''    if(replayID !== lastStrikeID)
    {
      el("graphSub").innerText = "Loading the strike from the rod\\u2026";
      await loadReplay(lastStrikeID);
    }

    el("graphTitle").innerText = "LAST STRIKE REPLAY";
    el("graphSub").innerText = replay && replayID === lastStrikeID ? replay.type + " · 4 s before and after"
      : replayFailed ? "Couldn\\u2019t load it \\u2013 tap LAST STRIKE to try again"
      : "No strike recorded yet";''')

rep('''    if(view === "strike" && d.strikeready && replayID !== d.strikeid)
    {''', '''    if(view === "strike" && d.strikeready && replayID !== d.strikeid && Date.now() - replayTryAt > 5000)
    {''')

open(path, 'w').write(s)
print("ok", path)
