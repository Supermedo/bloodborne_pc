# bbbridge (C++)

Play **co-op with bbhost players** on **The Hunter's Dream** while using
**bbport** (Supermedo's `bloodborne_pc` for Windows, DLSS / FSR 3.1 / FSR 4).

bbbridge looks like a shadNet server to bbport and talks to The Hunter's Dream
the way bbhost does: matching (rooms, bells, summons), the P2P traffic between
the two clients (different packet framing, bbport's signaling handshake,
The Hunter's Dream's STUN and relay), and the game's own server traffic
(account token, play log for the live map). bbport itself is not modified.

## Layout

```
E:\bloodborne\               bbport
└─ bbbridge\                 this folder
   ├─ bbbridge.exe           the app (built)
   ├─ bbbridge_config.json   settings (optional)
   ├─ bbbridge_state.json    your account: token + recovery code - never share it
   ├─ bbbridge.log           log of the last run (previous: bbbridge.prev.log)
   ├─ src\                   C++ sources
   ├─ tests\                 unit tests (C++) and end-to-end tests (Python)
   ├─ third_party\           nlohmann/json 3.11.3, Dear ImGui 1.91.5
   └─ bin\                   bbbridge-cli.exe (headless), bbbridge-unit.exe
```
<img width="798" height="718" alt="resim" src="https://github.com/user-attachments/assets/672a0038-4ead-4572-9293-b6c3936ed128" />


## Using it

1. Put the `bbbridge` folder inside bbport's folder (next to `Bloodborne.exe`)
   and start `bbbridge.exe` (close the bbport launcher first). bbbridge uses
   the bbport it sits in; anywhere else it asks which bbport to use
   (**Change...** picks another one later).
2. **Sign in with Discord**: the app shows a code and opens The Hunter's Dream;
   sign in there with your Discord account and approve the code. Your account
   there (the same one bbhost uses) becomes bbport's online id. No password is
   stored; the account's token goes to `bbbridge_state.json`.
3. The **Online** switch goes to **The Hunter's Dream (bbhost)** by itself
   once you are signed in (and the live map patch is added; untick it if you
   like).
4. Press **PLAY**. The app starts the bridge if needed and launches the game
   with bbport's saved settings. You can also start the game from the bbport
   launcher - as long as bbbridge is open. Graphics, mods and cheats stay in
   the bbport launcher (**bbport settings...**) or the in-game menu (Insert).

Move the **Online** switch to **shadNet** to go back to bbport's own shadNet
co-op pool: bbport gets its own server and shadNet account back (the ones it
had before; a shadNet account is registered at
[shadnet.shadps4.net](https://shadnet.shadps4.net) and entered in the bbport
launcher's *Online co-op*). PLAY follows the switch: right, the bbhost pool
with your Discord sign-in; left, bbport's own shadNet - no sign-in needed.
Running bbhost and bbport with the same account at the same time is not
supported.

## Troubleshooting

- **"Failed to acquire Bloodborne server information" with bbbridge closed**:
  bbbridge 0.3.1 and older (and the Python 0.2.0) left
  `user\host_overrides.custom.json` in bbport's folder (bbport's user folder),
  which sends the game's server traffic to bbbridge even when it is not
  running. Start bbbridge 0.3.2 or newer once (it removes the file when it is
  not needed), or delete that file by hand. From 0.3.2 the file exists only
  while bbbridge runs with the switch on The Hunter's Dream.
- **Nobody comes when I ring a bell**: a Small Resonant Bell is answered only
  by a host ringing the Beckoning Bell in the same area, near your level (and
  the other way round). The live map at
  [thehuntersdream.com/map](https://thehuntersdream.com/map) shows who is
  ringing which bell where.

## Building (Visual Studio 2022/2026)

Workload **Desktop development with C++** (MSVC, Windows SDK, CMake tools).

- Visual Studio: *File > Open > Folder...* this folder, pick the **x64 Release**
  preset, *Build > Build All*. `bbbridge.exe` is copied here, the CLI and the
  unit tests to `bin\`.
- Command line (Developer PowerShell):
  `cmake --preset x64-release` then `cmake --build --preset x64-release`

## Tests

- `bin\bbbridge-unit.exe` - framing, signaling, STUN, protobuf, account id.
- `python tests\e2e\test_e2e.py` - the bridge exe against a fake bbhost
  server, a fake STUN/relay server and fake game sockets (no internet).

## Credits and license

The protocol comes from **bbhost** and The Hunter's Dream by droogie (GPL-3.0);
**bloodborne_pc / bbport** by Supermedo on deadinside28's port and shadPS4.
bbbridge is GPL-3.0-or-later. nlohmann/json and Dear ImGui are MIT-licensed.
Not affiliated with Sony, FromSoftware, Supermedo or The Hunter's Dream.
