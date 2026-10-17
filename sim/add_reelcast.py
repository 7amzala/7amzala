#!/usr/bin/env python3
"""LIVE ROD: REEL IN and cast animations.

REEL IN: the reel handle turns, the rig comes in through the surf and up
to the tip, then the lead hangs from the tip while the rod is reeled in.
READY: the rod swings back, whips forward, the lead flies out in an arc,
splashes down and the line tightens.

They also play when the rod's state changes from elsewhere (another
phone, the watch). Run after add_rodview_v2.py.

    python3 add_reelcast.py <sketch.ino>      (edits in place)
"""
import sys

path = sys.argv[1]
s = open(path).read()


def rep(old, new):
    global s
    n = s.count(old)
    if n != 1:
        sys.exit(f"{path}: expected 1 x {old[:70]!r}, found {n}")
    s = s.replace(old, new)


rep('''let rodLastFrame = 0;
''', '''let rodLastFrame = 0;
let rodAnim = null;              // { mode: "reel" or "cast", start: ms }
let rodPrevState = "";

const ROD_REEL_MS = 4500;
const ROD_CAST_MS = 3400;

function startRodAnim(mode)
{
  rodAnim = { mode: mode, start: performance.now() };
}
''')

rep('''  rodKnockID = d.knockid;
''', '''  rodKnockID = d.knockid;

  // Reeled in or cast from somewhere else (another phone, the watch)
  if(rodPrevState && rodPrevState !== d.state)
  {
    const fresh = rodAnim && performance.now() - rodAnim.start < 8000;

    if(d.state === "paused" && !(fresh && rodAnim.mode === "reel"))
      startRodAnim("reel");
    else if(rodPrevState === "paused" && !(fresh && rodAnim.mode === "cast"))
      startRodAnim("cast");
  }

  rodPrevState = d.state;
''')

rep('''  drawRodScene(ctx, w, h,
  {''', '''  let mode = "normal";
  let anim = 0;

  if(rodAnim)
  {
    const e = performance.now() - rodAnim.start;
    const dur = rodAnim.mode === "reel" ? ROD_REEL_MS : ROD_CAST_MS;

    if(e < dur)
    {
      mode = rodAnim.mode;
      anim = e / dur;
    }
    else
    {
      rodAnim = null;
    }
  }

  if(mode === "normal" && d && d.state === "paused")
    mode = "out";

  if(mode !== "normal")
  {
    state = "armed";
    label = mode === "reel" ? "REELING IN…" : mode === "cast" ? "CASTING…" : "REELED IN – cast, then press READY";
  }

  drawRodScene(ctx, w, h,
  {
    mode: mode,
    anim: anim,''')

rep('''async function reelIn()
{
  stopContinuousAlarm();''', '''async function reelIn()
{
  stopContinuousAlarm();
  startRodAnim("reel");''')

rep('''async function ready()
{
  stopContinuousAlarm();''', '''async function ready()
{
  stopContinuousAlarm();
  startRodAnim("cast");''')

rep('''    knock: rodKnock,''', '''    knock: mode === "normal" ? rodKnock : 0,''')

open(path, 'w').write(s)
print("ok", path)
