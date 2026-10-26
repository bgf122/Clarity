# Changelog

Each release's section here becomes its notes on GitHub and in AoForge.

## 0.1.13

### Bars
- **Your target's target:** a small bar for whoever your nano target is targeting, which can be you.
  - It shows its level, name and health like the other target bars.
  - You can drag it anywhere.
  - Turn it on or off with "Bar for your nano target's target" in Options > Clarity > Bars.

### Nameplates
- **Smoother in raids:** each nameplate's health bar is updated at most ten times a second, so many characters losing health at once no longer costs frames.

## 0.1.12

### New: health, nano and target bars
Clarity can now show its own bars in place of the game's. They're off until you turn them on in **Options > Clarity > Bars**.
- **Your bars:** health over nano, in your interface skin's colours, with health in a deeper red.
  - Health you just lost shows as a lighter piece that holds a moment, then shrinks away.
- **Fight target:** its own bar, with a slowly pulsing orange outline so you always know who you're fighting.
- **Nano target:** a bar of its own.
  - While you have a fight target, it's a little smaller and sits apart.
  - When you don't, it takes the fight target's place at full size.
- **Names and levels:** above each target bar, coloured by how its level compares to yours, as the game rates it.
- **Numbers:** each bar shows the amount, the percent, or both. Very large amounts are shortened, for example 1.0B.
- **Moving them:** drag any bar where you want it. It stays there next time.
- **The game's bars:** they're hidden while Clarity's are on, and come back when you turn Clarity's off.
- **Settings:** a new **Bars** page in Options > Clarity. You can turn the bars on or off, pick the numbers, show or hide the target bars, lock the bars in place, turn the lost-health effect on or off, and set the size.

### Nameplates
- **Smoother in big fights:**
  - Health changes on nameplates update much more cheaply than before.
  - The blinking brackets on characters attacking you do too.

## 0.1.11

### Nameplates
- **Classic style** (Options > Clarity > Nameplates > Style): the game's own name tags, drawn sharp at any size.
  - They show the name only, in bold.
  - Your nano target gets teal corners at the top and your fight target red corners at the bottom; one character can have both.
  - Target plates get a thin health bar.
  - They sit and move exactly like the game's tags. Only their size and sharpness differ.
- Names now look as the game shows them:
  - Colours written into a name (for example a pet named in colours) show.
  - PvP-flagged players get `** Name **`. Players the game marks get `= Name =`.
  - The title comes first, on the name line. The second line holds only the org.

## 0.1.10

### Nameplates
- Your nano target's and fight target's plates show through walls, buildings and terrain.
- In front of the camera, they also stay inside the window. A target off the side, top or bottom of the screen keeps its plate at the edge, brackets included.

## 0.1.9

### Combat text
- **Other hit by nano** is back as an option (ShowOtherHitByNano), off by default.
  - It covers nano damage the game sends without saying whose it is.
  - It includes everyone's, so it floods the screen in raids.

## 0.1.8

### Nameplates
- **Health bar colour** has three choices:
  - **Classic**: the game's level colours.
  - **Single colour**: pick the hue with a slider.
  - **By health**: green at full health, yellow at half, red near death.

  The level number follows the bar's colour. A brightness slider goes with it; the default is 90%.

### Options
- Each page of the Clarity tab starts with **Default / Custom**. Choosing Default puts the page's settings back, and changing any setting switches it to Custom.

### Interface
- **Ctrl+Shift+U** hides the game's interface but keeps the nameplates and combat text. Press it again to bring the interface back. The key is `HideUiKey` under `[Interface]` in Clarity.ini.

## 0.1.7

### Nameplates
- Your nano target's and fight target's corner brackets are longer and thicker, with a solid outline.
  - The fight target is orange, so it stands apart from the red "attacking you" brackets.
- Target plates are a little bigger than the others. The slider is "Size of your target's plate" (TargetScale); the default is 110%.
- A faint dark panel behind the level, name and health bar makes plates stand out on bright backgrounds. The slider is "Background behind the plate" (BackgroundOpacity); the default is 10%.
- **Show own nameplate** (ShowSelf, off by default) puts a plate over your own character. It hides in first person.

## 0.1.6

### Nameplates
- Steadier target switching: a plate that just disappeared, such as your old target's, no longer pushes a new plate aside.
- The Devileet vanity pet counts as a pet. `PetFamilies` in Clarity.ini now adds to the built-in list.

### Combat text
- Combat text starts above the target's nameplate instead of on it.

### Options
- The Clarity tab also shows when MerkUI is installed.

## 0.1.5

### Nameplates
- Calmer stacking:
  - Stacked plates stay stacked only while they are close to each other.
  - They move up at most two plate heights, so crowds don't build towers.
  - They settle back faster.

### Combat text
- Special attack, perk and taunt names ("Fling Shot!", "Thunder") are about the game's own size. The size slider is ActionScale; the default is 0.55.

## 0.1.4

### Nameplates
- **New layout:**
  - The full name sits centred over a health bar that stretches with it.
  - The level is in bold to the left, in the bar's colour.
- Plates hide behind walls and terrain, and are drawn over other characters instead of behind them.
- Characters attacking you get slowly blinking red brackets outside the target brackets. Every plate keeps room for both, so plates don't move when a character's role changes.
- Stacked plates stay stacked while close and briefly after parting, and settle back slowly, so moving groups don't make them jump.

### Combat text
- Nano damage from an unknown source is never shown. The `ProcWindowSec` setting is removed.

## 0.1.3

### Nameplates
- Plates hide behind walls like the game's name tags.
- The nameplate `OnTop` setting is removed. Floating combat text keeps its own.

## 0.1.2

### Fixes
- Fixed a crash at the character selection screen when Clarity ran next to other mods, such as MerkUI.

## 0.1.1

### Combat text
- **"Miss"** and **"Evade"** show for missed attacks.
- Heals show in green. Nano gains show in blue: nanopool increases, nano given to you, and nano drains.
- Heals and nano you give show over the person you gave them to.
- Numbers over your own character get -/+ signs (SelfSigns).
- Texts start side by side by their width and rise straight up from where they appeared (DriftPx 0; FollowTarget is off by default).
- Other players' special attack, perk and taunt names are hidden (OthersSpecials).

### Nameplates
- With ao-vk, plates are drawn after its shadows and effects.
- Plates no longer bob with walk and run animations, and overlapping plates keep their order.
- Types you switch off show the game's own tags.
- Your selected and fight targets' plates don't fade beyond the 30 m tag range.

## 0.1.0

The first release:
- floating combat text;
- nameplates;
- a Clarity tab in the game's Options window.
