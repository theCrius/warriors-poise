# Warriors' Poise

An Enshrouded mod for [ShroudForge](https://github.com/bonsaibauer/shroudforge) that makes
melee fights about standing your ground. It has three features. Each one is a separate switch
in the mod's ShroudForge settings, and all start **off**.

> **Beta.** Fully tested on game version 1287369 (the Steam update of 2026-11-04) with
> ShroudForge 1.7.9, in local worlds and with one player on a dedicated server. Not tested yet:
> several players at once, friends joining a game hosted from the client, and Medium hits, which
> no test has run into.

## Features

### Poise

Your armour and shield form a pool of poise. Light, Medium and Heavy hits drain it, and while
points remain they only make you twitch instead of interrupting you. When the pool runs out, hits
land as usual. It fills up again after 3 seconds without a hit. Knockback still happens, and Boss
hits, stuns and knockdowns always land.

| Worn | Poise |
|---|---|
| Heavy armour piece (Warrior) | 10 |
| Medium armour piece (Ranger, mixed sets) | 6 |
| Light armour piece (Mage, cloth, fur, starter) | 3 |
| Shield, raised or not (not with a two-handed weapon) | 30 |

| Hit | Cost |
|---|---|
| Light | 5 |
| Medium | 10 |
| Heavy | 50 |

So a full heavy set gives 50, or 80 with a shield: enough to push through a Heavy hit. All of
these numbers are sliders in the settings.

### Stronger armour

Multiplies the physical and magical defence of every armour piece, by 1.25 unless you change it.
Each armour type keeps its own balance between the two, and tooltips show the new values.

### Swing cancel

Block or dodge out of a melee attack during its wind-up, heavy attacks included. You stay
committed only during the swing itself, and you recover sooner after it.

## Requirements

- Enshrouded on Windows, and ShroudForge 1.7.9 or newer.
- With [Forged & Found](https://github.com/theCrius/forged-and-found), install either order: its
  armour variants get the same treatment.
- Multiplayer: install the mod on the dedicated server and on every player's game (see below).

After a game update the mod is built to keep working without an update of its own, and to step aside
safely when it cannot:

- Poise does not depend on fixed addresses. It finds the game code it needs by its shape, and takes
  the layout of the game's data (equipment, hits, hit types) from the type information ShroudForge
  extracts from the exact game version you run. If anything is missing, ambiguous or unexpected,
  poise stays off, hits behave as in the unmodded game, and
  `shroudforge/logs/warriors-poise-native.log` says why.
- Swing cancel and stronger armour change game data. If a game update changes that data in a way
  they do not expect, the feature is skipped with a warning in ShroudForge's log, and the other
  features and mods load normally.

## Install

1. Download the newest version:
   [warriors-poise.zip](https://github.com/theCrius/warriors-poise/releases/latest/download/warriors-poise.zip)
   (always the latest release; older versions are on the [releases](../../releases) page).
2. Copy its `warriors-poise` folder into `<game>/mods/` (next to `enshrouded.exe`).
3. Start the game, open the ShroudForge mod list and switch on the features you want.
   Changing a switch needs a restart.

Updating replaces the mod folder, which resets the switches to off.

## Dedicated server

The server needs ShroudForge installed beside `enshrouded_server.exe` (see ShroudForge's server
guide) and the `warriors-poise` folder in its `mods` folder. Hosts that only let you edit the save
files and the server config, and always start the official server program, cannot run mods at all.

**The server decides poise**, because it handles every hit, so poise needs its switch on on the
server. Keep all switches and sliders the same on the server and on every player's game: armour
tooltips, for example, come from the player's own data. Change the server's switches in either of
these ways, then restart the server:

- On a Windows server you can log into, start `shroudforge\shroudforge.exe` in the server folder
  (or double-click it), select "Warriors' Poise" and use its switches. From PowerShell, in the
  server folder:
  ```
  .\shroudforge\shroudforge.exe
  ```
- On any server whose files you can edit, open `mods/warriors-poise/extended.mod.json` and set
  `"value"` to `true` (on) or `false` (off) for `poise`, `armourDefence` and `swingCancel`. Example:
  ```json
  "poise": {
    "value": true,
  ```

Updating the mod on the server resets the switches to off, so set them again after each update.

## Removing the mod

Switch it off in ShroudForge or delete its folder, then restart. The mod saves nothing into your
characters or worlds, so there is nothing to convert or clean up.

## Building from source

Needs Windows, PowerShell 7 and **Visual Studio 2022 version 17.14** (the free Build Tools are
enough) with the **MSVC 14.44.35207** compiler:

1. Install [Build Tools for Visual Studio 2022](https://visualstudio.microsoft.com/downloads/) with the
   "Desktop development with C++" workload.
2. If your Visual Studio 2022 is newer than 17.14, add the individual component
   "MSVC v143 - VS 2022 C++ x64/x86 build tools (v14.44-17.14)" in the Visual Studio Installer.

The compiler is pinned on purpose: another MSVC version builds a different DLL from the same source,
and only a DLL that was tested in game should be released. With the pinned compiler, a local build
and GitHub's release build produce the same DLL. `native/build.ps1` stops with an error naming what
is missing rather than build with another compiler.

```powershell
./tools/package.ps1     # builds the DLL and writes dist/warriors-poise-<version>.zip
```

Pushing a tag `v<version>` matching `mods/warriors-poise/mod.json` builds the zip on GitHub
Actions and publishes it as a release.

| Path | Contents |
|---|---|
| `mods/warriors-poise/` | The ShroudForge package: `src/mod.lua` (attack timing, armour values, poise points) and the settings in `extended.mod.json` |
| `native/warriors-poise-native/` | The DLL that spends and refills poise on every hit (`autodetect.h`: finding the game code; `typedump.h`: reading the data layout) |
| `native/autodetect-test/` | Runs the DLL's detection on a game executable on disk: `native/build-autodetect-test.ps1`, then `autodetect-test <exe> [types-*.json]` |
| `tools/` | Packaging |

## License

GPL-3.0, see [LICENSE](LICENSE). Credits and third-party notices are in [NOTICE](NOTICE).
