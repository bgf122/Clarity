# Clarity: Anarchy Online client UI mod

Clarity is a 32-bit mod DLL that improves the AO client UI. It loads as a `dinput.dll` proxy.

Machine-specific notes (client paths, test setup) go in `CLAUDE.local.md`, which is not in git.

## Layout
- `src/`: mod source.
  - `proxy/` is the `dinput.dll` proxy.
  - `core/` holds the log, INI config, signature scan, MinHook wrapper and renderer access.
  - `features/<name>/` holds one folder per feature.
- `src/core/signatures.h`: byte signatures for non-exported client functions. `tools/verify_sigs.py <client dir>` checks that each one matches exactly once.
- `third_party/minhook`: vendored MinHook 1.3.4.
- Work on a **test copy** of the client, never on your real install. Re-confirm RE findings against the test client's bytes, e.g. with the Ghidra MCP servers in `.mcp.json` (copy `.mcp.json.example` and fill in your paths).

## Build and deploy
Use the CMake bundled with VS 2022, and target 32-bit (Win32):
```
"C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" -S . -B build -G "Visual Studio 17 2022" -A Win32 -DDEPLOY_DIR=<test client dir>
"...\cmake.exe" --build build --config Release
python tools/verify_sigs.py <test client dir>
```
- The post-build step copies `dinput.dll` to `DEPLOY_DIR` (empty = no copy). The game must be closed, because the DLL is locked while it runs.
- The version is `project(Clarity VERSION ...)` in `CMakeLists.txt`. It goes into the DLL's VERSIONINFO (ProductName "Clarity"), which the AoForge installer reads.
- Releases: pushing a `v*` tag runs `.github/workflows/release.yml`, which builds and attaches `Clarity-<version>.zip`.

## Per-install AO data
AO keeps prefs, `DXDiagOutput.txt` and the GUI in `%LOCALAPPDATA%\Funcom\Anarchy Online\<hash>\client\`.
- **How the game computes `<hash>`** (`BinaryIO!AnarchyPath_t::GetUserDataPath`): it takes the parent of the client dir, from the ANSI cwd with `/` separators and no trailing slash, e.g. `C:/Project Rubi-Ka`. It hashes that with boost `hash_combine` over *signed* chars: `h ^= c + 0x9e3779b9 + (h<<6) + (h>>2)`. The result is printed with `%x`.
- The PRK launcher hashes the path differently. For ASCII paths the two agree, but with non-ASCII paths they don't, so keep test clients on an ASCII path.
- A fresh client copy has its own hash folder. Seed it from your real install's folder, or the game runs on defaults: a tiny window and the "display driver/device combination" warning.

## Testing
- Launch the game from the test copy and log in.
- `Clarity.log` in the client folder should contain `[hook] ... installed` and `[fct] ready`.
- Also check `AOReloaded.log` and `randy-vk.log` there for new errors.
- `Clarity.ini` is reloaded live (polled every 0.5 s).
- With `[FloatingText] Debug=1`, every floating text is logged with its colour index and projection values.

## How the mod loads, and coexisting with other mods
- **Loading.** The game exe is `AnarchyOnline.exe`. `Anarchy.exe` is the old patcher and is not used. `DisplaySystem.dll` imports `dinput.dll`, so our proxy loads in the game process only, not in CEF helpers. `DllMain` only starts a thread. Hooks go in after Gamecode/DisplaySystem/randy31 are loaded.
- **Other mods.** These may already be in the process. Do not hook their targets:
  - `version.dll` = AOReloaded (`AOReloaded.ini`): movement, input, inventory and skill-window hooks, `CreateFileW/A` import patches, and it patches the exe on disk.
  - `randy31.dll` = ao-vk (randy-vk Vulkan renderer): forwards data exports to `randy31_orig.dll`, and hooks `View::_CallRender` and `WindowController_c::Render`.
  - `aonewbie-hook.dll` (external, Detours): `N3Msg_*`, `DataBlockToMessage`, `TextView_c::SetText`, `RunEngine`.
- **Finding targets.** Prefer exports, resolved by mangled name. Only use signatures for non-exported functions. On a signature mismatch the feature disables itself and logs. Never patch blindly.
- **Address math.** Client DLLs have image base 0x10000000 but are relocatable. Always compute module base + RVA.

## Floating combat text (src/features/fct)
- **Created by:** Gamecode `CreateFloatingText(dynel, text, colorIdx)` (stdcall) → effect 0x2F5A `_GfxControlFont_t`.
- **Drawn by:** `DisplaySystem!GfxVisualDiaBill`, as a camera-facing billboard built from `DiaBill_n::Sprite_t` (0x20 bytes each) and the 128×64 `spritefont.png` atlas.
- **Our hooks:**
  - `SetText`: swaps in our sprite array (shadow + 8 outline + glyph layers) with `GfxVisualDiaBill::Init`.
  - `Process`: rewrites the array each frame for constant pixel size, using `RViewPort_t::m_CurrentViewMatrix`/`m_CurrentProjectionMatrix` and the window height.
- **Next milestone:** replace the 12 px aliased atlas with a high-res antialiased one. This needs RE of how `MMGetMaterial(40)` loads `spritefont.png`.

## Features
- `src/features/fct`: floating combat text. It uses exact crits and glancing hits (AttackInfo hit type at [ebp+0x20] in the combat function), drain heals (CharacterAction 0xD0), and hides others' damage by chat group (read from the caller frame).
  - The combat-message function (SIG_COMBAT_MESSAGE, `__thiscall(event, target, amount, attacker, special, dmgType, hitType, x)`, ret 0x20) makes floating text only for a non-zero amount; several cases leave through the exit that zeroes it, so Clarity adds text after the original: 0x3B miss, 0x41 you healed, 0x43 you gave nano, 0x44 got nano from someone. Events 0x42-0x44 are sent from the CharacterAction handler.
  - CharacterAction 0x8C = "You increased your nanopool with %d points" (amount in p28[1]).
  - Names shown over the performer: special attacks colour 13, perks/actions 3, taunts 2 (hidden over other players unless OthersSpecials).
  - With `Debug=1`, every combat event and CharacterAction is logged.
- `src/features/nameplates`: the game's nametag Indicators get our own VisualMaterial_t texture: level + name, an org/title line, and a health bar. Sizing is per frame from the RSprite vertices. Per-type visibility uses VisualSprite_t::SetInvisible after the game's update. The Indicator vtable slot 0 is hooked to free our texture. The selected target's and fight target's indicators (+0x31 / +0x30) get the same plate with corner brackets (white / red-orange; gold and a single plate when it's the same character). Turning nameplates off restores the game's material and quad on every Indicator.
  - Plates are drawn without depth test and hidden behind walls/terrain by `N3!n3Playfield_t::LineOfSight(from, to, zone -1, false)` from the camera (CameraWorldPos from the view matrix), tested every 100 ms per plate.
  - Characters attacking us (core/threat: damage on us with a known attacker, misses on us, the game's "Attacking You!") get blinking red brackets outside the target brackets. All plates reserve room for both bracket sets.
  - Texture 1024x256 (long names at full size; ao-vk's label test needs width >= 2x height).
- `src/features/settings`: the Clarity tab in the options window. It registers DValues `CLR_*` with ranges, reads changes with a DistributedValue_c observer, and serves Root.xml by chaining on msvcr100's CreateFileA/W import slots after AOReloaded.

## Backlog
- **Nameplates: team visibility toggle.** Not done yet: N3Msg_GetTeamMemberList / N3Msg_IsInTeam semantics are unverified.
