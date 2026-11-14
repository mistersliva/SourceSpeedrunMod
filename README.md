# SourceSpeedrunMod

SpeedrunMod for Source-based mods. Based on [Obsoletium](https://github.com/Source-Authors/Obsoletium)
(64-bit Source engine), playing **Half-Life 2** content from your Steam install.

> You need Half-Life 2 on Steam. The mod reads the game content from your Steam
> library; no Valve game content is included here.

## Features

- **New game UI in HTML/CSS/JS** (Chromium Embedded Framework) replacing `GameUI.dll`:
  main/pause menu, settings, new game, load/save, console, loading screen.
  Source: [`gameui/`](gameui/) (engine side) and [`gameui/htmlui/`](gameui/htmlui/) (pages).
- **HTML HUD**: health/armor, ammo, timer and speed panel, weapon selection,
  damage indicators. Everything can be customized in settings (scale, opacity, accent,
  visible parts).
- **Switchable movement**, in game or bound to keys (`movement_hl2`, `movement_oe`, `movement_hl1`, `movement_cycle`):
  - Half-Life 2: accelerated back hopping (ABH),
  - Half-Life 2 old engine: accelerated forward hopping (AFH),
  - Half-Life 1 movement (port of an SDK 2013 HL1 movement mod: 800 gravity, 320 run,
    30 air acceleration, HL1 jump and duck).
- **Auto bunnyhop** (`sv_autobunnyhopping`, `autobhop_toggle`).
- **LiveSplit-style timer** with `.lss` splits (`hl2/splits/`): game time without
  loads, auto start / auto split, best segments and PBs saved back to the file.
  Hotkeys: `timer_split`, `timer_reset`, `timer_undo`, `timer_skip`.
- Frosted-glass blur behind menus, faster loads (`cl_load_settle_time`), the last
  frame stays on screen during save/loads instead of going black.
- **Launcher** ([`ssm-launcher/`](ssm-launcher/)): Electron app that finds Steam and
  Half-Life 2, checks ownership after "Sign in through Steam", points the mod at your
  HL2 install and downloads mod updates.

## Install (players)

1. Install **Half-Life 2** in Steam.
2. Download **`SSM-Launcher-Setup.exe`** from [Releases](../../releases) and run it.
3. In the launcher press **Install** (it downloads the mod), then **Play**. Updates show up as **Update**.

## Build (developers)

The engine builds like upstream Obsoletium; see [BUILDING.md](BUILDING.md) and
[README.Obsoletium.md](README.Obsoletium.md). For HL2 x64:

```
create_game_projects.bat hl2 x64
build_htmlui_thirdparty.bat x64    :: downloads CEF and builds the gameui CEF bridge
msbuild hl2_x64.slnx /p:Configuration=Release /p:Platform=x64
```

The HTML UI is documented in [`gameui/README.md`](gameui/README.md), the launcher in
[`ssm-launcher/README.md`](ssm-launcher/README.md).

## License

Engine code: see [LICENSE](LICENSE) (Source SDK license, as in Obsoletium). Half-Life,
Valve and Steam are trademarks of Valve Corporation. This project is not affiliated with Valve.
