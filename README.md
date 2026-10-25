# Clarity

Clarity is a UI mod for the Anarchy Online client. It's one DLL (`dinput.dll`) that you drop into the client folder.

## Features
- **Floating combat text.**
  - Large, outlined damage and heal numbers that stay the same size on screen at any distance.
  - Crits pop and shake, and glancing hits are drawn smaller. Special attack, perk and taunt names ("Fling Shot!") are small, about the game's own size.
  - Heals in green, nano in blue; "Miss" when your attack misses and "Evade" when an attack on you misses.
  - Things the game only puts in chat get numbers too: nano you gain, heals and nano you give, health and nano drains.
  - Numbers on you are signed: -damage taken, +heals and gains.
  - Several hits on one target start side by side and rise straight up from where they happened.
  - Per category, you choose whose numbers you see (yours, your pet's, other players'). Other players' special attack and perk names are hidden, and nano damage with no known source is never shown.
- **Nameplates.**
  - Readable plates over players, NPCs and pets: the full name centred over a health bar that stretches with it, the level in bold to the left in the bar's colour (classic level colours, one colour of your choice, or by health), and an org/title line, on a faint dark background (adjustable) so they stand out against bright scenery.
  - Your selected (nano) target and your fight target get bold brackets (white, orange, gold when they are the same character), are drawn a little bigger than other plates (adjustable), and stay visible beyond the 30 m tag range.
  - Optionally a plate over your own character (hidden in first person).
  - Characters attacking you get slowly blinking red brackets.
  - Classic style: the game's own look (name only, teal corners on your nano target, red on your fight target, a thin health bar on targets), drawn sharp at any size, with the same sizing, stacking and wall hiding.
  - Ctrl+Shift+U hides the game's interface (windows, chat, bars) and keeps the nameplates and combat text, e.g. for screenshots. The key is set in Clarity.ini (`[Interface] HideUiKey`).
  - Overlapping plates are moved apart calmly and keep their order, they don't bob with walking, and you can click a plate to select that character.
  - Like the game's own tags, plates are hidden behind walls and terrain, but not behind other characters.
  - Switch players, NPCs or pets off to get the game's own tags for them.
  - Works with ao-vk: plates are drawn after its shadows and effects.
- **Settings tab.** The game's Options window gets a **Clarity** tab, and changes apply immediately. Everything is also in `Clarity.ini`: fonts, colours and the finer settings.

## Install
**With [AoForge](https://github.com/bgf122/AoForge) (recommended):** install Clarity from the addon list. AoForge also handles updates and uninstalling.

**By hand:**
1. Download `Clarity-<version>.zip` from [Releases](../../releases).
2. Close the game.
3. Copy `dinput.dll` into the client folder, the one that contains `AnarchyOnline.exe`.
4. Start the game. Clarity creates `Clarity.ini` and `Clarity.log` next to the DLL.

If the folder already has a `dinput.dll` from another mod, don't overwrite it. Clarity and that mod can't both use the name.

## Uninstall
Close the game and delete `dinput.dll`. You can also delete `Clarity.ini` (your settings) and `Clarity.log`.

## Compatibility
- **Works alongside** [AOReloaded](https://github.com/wannabuh/AOReloaded) (`version.dll`) and [ao-vk](https://github.com/wannabuh/ao-vk) (`randy31.dll`).
- **Client updates.** Clarity finds some game functions by their byte patterns. If a client update changes them, the affected features switch themselves off and say so in `Clarity.log`, and the game keeps running normally. A Clarity update fixes that.
- It only loads into `AnarchyOnline.exe`.

## Settings
Open **Options → Clarity** in game, or edit `Clarity.ini` while the game runs: the file is reloaded automatically. The sections are:
- `[FloatingText]`: sizes, timing, outline, which categories are shown, font;
- `[Colors]`: per-colour overrides;
- `[Nameplates]`: sizes, what's shown, opacity, font.

If something looks wrong, set `Debug=1` under `[FloatingText]` and include `Clarity.log` in your report.

## Building from source
Requirements: Visual Studio 2022 with the C++ workload. The DLL is 32-bit.
```
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32 -DDEPLOY_DIR=<test client folder>
cmake --build build --config Release
```
- `DEPLOY_DIR` is optional. When it's set, the built `dinput.dll` is copied there.
- `python tools/verify_sigs.py <client folder>` checks the byte signatures against a client. It needs `pip install pefile`.
- Pushing a `v*` tag builds a release on GitHub Actions.

## Licence
MIT, see [LICENSE](LICENSE). Clarity includes MinHook; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Clarity is a fan project. It is not affiliated with or endorsed by Funcom. Anarchy Online is a trademark of Funcom.
