# Changelog

## 0.3.2 - 2026-10-12

- **Fix: "Failed to acquire Bloodborne server information" after closing
  bbbridge.** The file that sends the game's server traffic to bbbridge
  (`host_overrides.custom.json` in bbport's user folder) now exists only while
  bbbridge runs with the switch on The Hunter's Dream: it goes when bbbridge
  closes, when the bridge stops, or when bbport is set back to shadNet - also
  in the bbport launcher itself. One left behind by an older bbbridge
  (including the Python 0.2.0) is cleaned up the next time bbbridge starts.
- Troubleshooting notes in the README.

## 0.3.1 - 2026-10-12

- **Online switch** in place of the "Route bbport through bbbridge" box:
  shadNet on the left, The Hunter's Dream (bbhost) on the right. A line under
  it says which pool and account PLAY uses.
- **shadNet comes back whole**: switching to shadNet restores bbport's own
  server *and* shadNet account (name, password). The Hunter's Dream account is
  bbport's online id only while switched to it. PLAY follows the switch; on
  shadNet no Discord sign-in is needed.
- **Set up by default**: once signed in with Discord, the switch goes to The
  Hunter's Dream and the live map patch is added; turning either off sticks.
- **bbport folder**: shows where it was found; bbbridge.exe right next to
  Bloodborne.exe works too; asks when it had to guess, when a chosen folder is
  gone, or when bbport is not found.
- **Clear errors**: PLAY and "bbport settings..." say what is missing (sign-in,
  game folder, shadNet account, launcher open, missing exe, Windows' error).
  "bbport settings..." also works before the launcher has any settings.
- **Lighter**: the window draws at full rate only while used (idle CPU about a
  quarter of before).
- **Steadier event poll**: it no longer stalls ~8 s about once a minute, so a
  summon reaches the game without that delay.
- Verified live: joining a bbhost host's world as a guest (Small Resonant Bell).

## 0.3.0 - 2026-10-12

- First C++ release: one window for the bridge, Discord sign-in, routing bbport,
  the live map patch and starting the game.
