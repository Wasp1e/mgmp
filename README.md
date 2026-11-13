# mgmp — co-op for Mewgenics

An unofficial mod that lets two people play one Mewgenics campaign together:
the same House, the same runs, the same cats and the same fights, each player
in charge of their own half of the cats.

Version **0.25** (network protocol 83), for Mewgenics **1.1.21239**.

This is a fork of [SanTertrust/mgmp](https://github.com/SanTertrust/mgmp),
which laid the foundation: the battle lockstep, the network layer and the
first co-op runs. Everything listed below grew on top of it.

It is not an official feature and has nothing to do with the developers of
Mewgenics. It is a hobby project built by reading the game's compiled code.

---

## What it does

**Connecting**
- Through Steam: no port forwarding, no VPN, the two of you only need to be
  Steam friends. Plain TCP still works for a LAN or a virtual network.
- The joining player stays on the main menu: the host's save arrives, the mod
  presses Play and loads it. The friend's own saves are never touched.

**The House**
- The friend's game follows the host's House: NPC offices and their dialogs
  (the host's clicks move them on both screens), quest rewards, cutscenes.
- After the end of the day both players get the night's report — who bred,
  who fought, which kittens were born — on a torn paper note.
- Only the host ends the day.

**Setting off**
- Every player picks cats for the run by dragging them into the House's box;
  the box is the same on every screen, and the cats a player put in are that
  player's for the run. A friend puts in up to two cats with two players, one
  each with three or four (the host two with three); the note in the top right
  corner counts them.
- The game's own screens, on both sides: the class chooser, then the gear
  screen. The cats are split evenly; each player puts collars and gear on
  their own cats, the other player's cats are dimmed, and every move shows up
  at once on both screens. Items never end up in two places.
- The same for the House defense (the guillotine and the other House bosses).
- The embark cutscene plays on both.

**A run**
- The map follows the host; a node picked by both players makes no extra meow
  (and there is a volume slider for the meows in F1).
- The backpack works move by move, with either backpack open: each player
  dresses their own cats.
- Events, level-ups and shop level-ups offer the same options on both screens,
  and each player picks for their own cats. A level-up reroll (the d6) is the
  cat owner's and plays on both screens.

**Fights**
- Lockstep: only the players' clicks travel, both games roll the same dice and
  reach the same result. The other player's cat has its action bar greyed out.
- You see your partner's cursor and what they are aiming at before they commit.
- In a fight, double-click a tile during another player's turn to ping it: it
  flashes in your colour on every screen for four seconds, with a sound.
- Notes on screen: "Another player's turn", "Waiting for another player — not in
  the fight yet".
- A friend who drops can rejoin in the middle of a fight. A fight loaded from a
  save waits for both players before the first move.
- Every turn both games compare a fingerprint of the fight. A small difference
  (health, the dice stream) is corrected to the host's; anything bigger stops
  both games (HALT) rather than let them drift apart, and the friend's log is
  sent to the host automatically.

**Languages**
- The mod's own words follow the game's language: Russian when the game is in
  Russian, English otherwise.

**Still rough**
- Two players is what has been played the most. Three and four connect, split
  the cats and share the backpack and gear screens, but have only been tried
  locally so far.
- It is pinned to one version of the game (see
  [When the game updates](#when-the-game-updates)).
- Expect bugs. Report them with the logs of **both** players.

---

## What you need

- **Windows**, 64-bit.
- **Mewgenics** `1.1.21239` from Steam, the same build on both machines. A
  different build is noticed and refused, since it would drift apart.
- Steam running, and the two players in each other's friends list (or a
  network you can both reach, for TCP).

To build it yourself you also need **Visual Studio 2022/2026** (or the MSVC
build tools) and **CMake 3.21+**.

---

## Getting it running

### Prebuilt

Download the archive from [Releases](https://github.com/Wasp1e/mgmp/releases),
unpack it anywhere **outside** the game's folder and run `mgmp_loader.exe`.
Do not run it from inside the archive. `README.txt` and `README_RU.txt` in the
archive are a short guide, in English and in Russian.

Windows Defender may complain: the loader puts a DLL into the game, which is
how mods work. If `mgmp_loader.exe` disappears, restore it from Defender's
protection history and add the mod's folder to the exclusions.

### Build

```powershell
cmake -B build -A x64
cmake --build build --config Release
```

That produces, in `build\Release\`:

| file | what it is |
|---|---|
| `mgmp_loader.exe` | what you run. Starts the game with the mod loaded. |
| `mgmp.dll` | the mod itself. The loader puts it into the game. |
| `mgmp.json` | your settings. Created the first time. |

They sit in the same folder. Dear ImGui and MinHook are compiled into
`mgmp.dll`; there is nothing else to install.

The loader finds the game through your Steam libraries and creates
`steam_appid.txt` next to `Mewgenics.exe` if it is missing. If it cannot find
the game, set `game` in `mgmp.json` (doubled backslashes in JSON paths).

---

## Playing together

1. Both players start Steam, then `mgmp_loader.exe`.
2. Press **F1** for the mod's panel.
   - The host ticks **through Steam**, presses **host** and sends **your ID**
     to the friend.
   - The friend ticks **through Steam**, pastes it into **host's ID** and
     presses **join**.
3. The host picks a save slot. The friend's game loads the same campaign on
   its own, into a separate `mgmp_coop.sav`.
4. Play. On your cat's turn you move it; on your partner's you watch. Cats run
   by the computer are handled by both games identically.

The host owns the campaign: only the host's save is written. Before replacing
the coop save the mod keeps the previous one in `saves\mgmp_backups`.

### Over TCP instead of Steam

Set `"transport": "tcp"` in `mgmp.json`. The host listens on port **27600**;
the friend sets `addr` to the host's address. On one network that is the
host's local IP; over the internet the host forwards TCP 27600, or you both
join a virtual network (Tailscale, ZeroTier, Radmin, Hamachi).

---

## While you are playing

### The panel (F1)

Connection, who controls which cat, how many turns have been agreed on, a live
log, the meow volume, and a button to fetch the friend's log (the host) or send
yours (the friend).

### Logs

Every session writes `logs\mgmp_trace_<date>-<time>.log` next to the loader.
When a fight halts, the friend's log is sent to the host by itself and lands in
`logs\friend`. For a bug report, the logs of both players are what make it
diagnosable.

---

## Settings

Everything lives in `mgmp.json`, beside the loader.

| setting | what it does |
|---|---|
| `game` | full path to `Mewgenics.exe` (found on its own if empty) |
| `net.role` | `off`, `host` or `client` (the panel sets it too) |
| `net.transport` | `steam` or `tcp` |
| `net.addr` | the host's SteamID64 (steam) or IP (tcp) for the friend; what to listen on for the host |
| `net.port` | default `27600` |
| `net.control` | which cats you control. Leave on `"auto"`: it splits them evenly. |
| `ui.enabled` | whether the panel exists at all |
| `ui.visible` | whether it starts visible |
| `ui.key` | key to toggle it, default `F1` |

The `debug` block is for development and best left alone. Everything else is a
compile-time constant in `src/core/mgmp_tuning.h`.

---

## When the game updates

The mod finds the game's functions by signature, so a small patch is often
survived on its own; the log says how many addresses resolved and whether any
moved. A big patch can still break it until the mod is updated. Both players
must run the same game build.

---

## How it works, briefly

Keeping two games in step is two problems with opposite answers.

**Fights are deterministic.** Given the same start, the game plays out the
same way down to every dice roll — measured, not assumed. So the mod sends
only what each player clicked, and both games work out the consequences.

**Everything outside fights is decided by the host.** The map, events,
level-ups, the House: the host's game decides and the friend's follows, either
by the same input on the same screen or by the host's data (cats, inventory,
run history, save) sent across.

Both halves are checked all the time: every turn of every fight and every map
node, the two games compare a fingerprint of their state.

The full technical story — addresses, offsets, measurements and dead ends — is
in `CLAUDE.md` and in the comments next to the code.

---

## Project layout

```
src/core/          loading, hooking, logging, config, crash reports, language
src/determinism/   random number tracking, ability identity, recording and replay
src/net/           the network transport (TCP and the Steam tunnel) and the messages
src/session/       everything that keeps the two games in step
src/ui/            the F1 panel, notes and the on-screen overlay
loader/            mgmp_loader.exe
tests/             unit tests for the logic that can be tested alone
tools/             PowerShell and Python helpers, net_test.ps1 for two local games
third_party/       Dear ImGui, MinHook, stb_image, nlohmann/json
```

Run the tests with:

```powershell
ctest --test-dir build --build-config Release
```

---

## A note on what this is

This mod contains no Mewgenics code, art or data. It is an independent program
that attaches to a legally purchased copy of the game on your own machine.

It is not affiliated with, endorsed by, or supported by the developers or
publisher of Mewgenics. Back up your saves before using it. If you report a bug
to the game's developers while the mod is loaded, please say so.

Mewgenics is by Tyler Glaiel and Edmund McMillen. It is a very good game and
you should buy it.

Licensed under the MIT License — see `LICENSE`.
