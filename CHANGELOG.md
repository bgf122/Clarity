# Changelog

Each release's section here becomes its notes on GitHub and in AoForge.

## 0.1.19

### Frames
- **Your level** shows in teal before your name.
- **Attack rating and Melee/ma AC** under your nano bar, in the colours of the game's Stats window.
  - **All your armour classes:** rest the mouse on the Melee/ma AC line and a small box lists all eight.
- **Your health and nano bars** have the same shading as the bars in the game's Stats window.
- **A cleaner look:** the silver outer border is gone from every frame, there is less empty space inside them, and the fine stripes in the bars are gone.
- **Targets with no health** (objects, corpses) get a slim frame under their name instead of an empty bar.

### Floating Combat Text
- **Fixed:** nano that other players gained showed up as floating text, as if you had gained it.

### Minimap
- **New frame:** the silver rim is gone. The ring is thicker, and the compass letters, buttons and markers sit on it.
- **The game's compass** is hidden while the minimap is on, and comes back when you turn the minimap off.
- **Dungeons on your planet map:** some planet maps draw dungeons too (Saavick's map has Temple of Three Winds, Smugglers Den, Camelot Castle and more). In those, the minimap switches to the planet layer by itself. The layers button switches back for that visit.

## 0.1.18

### Minimap
- **Turret cones:** turrets show the red cone of what they cover, as on the game's own map, instead of a small arrow.
- **Your selected target's arrow** now shows on every floor of a dungeon, as on the game's map.

## 0.1.17

### Minimap (new, off by default)
- **A round minimap** in place of the game's local map: Options → Clarity → **Minimap**.
  - It shows what the game's local map shows, at the same scale: dungeon rooms, the ground outdoors, and the game's own markers and arrows, updated as often as the game's.
  - **Compass:** N, E, S and W on the frame, a light marker on the outer rim pointing the way you face, and a gold marker on the inner ring pointing at your mission when it's in the same playfield.
  - **+ / −** zoom from 75% to 150%.
  - **The layers button** switches to the planet map you picked in the game's planet map window. There, + and − step through its levels like the planet map's own zoom. Inside a building or dungeon it shows where you went in.
  - **Moving it:** drag it anywhere. Once it's locked, dragging inside it looks around the map, and a click centres it on you again.
  - **The game's map button and key** show and hide it, the same for all your characters. The game's own local map window stays closed while the minimap is on. The planet map still opens as before.
  - Settings: Enabled, Locked, Size and Opacity.

### Frames
- **Movable timer bars (off by default):** untick **Timer Bars Locked** to drag the game's timer bars (attack, nano, perk, item...) anywhere. While they're unlocked, a gold bar shows where they start, so you can move them without an action running. Lock them again once they're placed.

### Misc
- **Show Alien Mission Layout (off by default):** in alien mission zones the game draws the map's rooms black on the black map. Clarity draws them in a dark green, with or without ao-vk.

## 0.1.16

### Nameplates
- **Styles:** pick **Classic** (the framed plates, default) or **Modern** (the 0.1.14 look: the level beside the name, a health bar with the percentage on every plate, corner brackets on your targets, in Verdana).
- **Modern options:** Background Opacity, Enable Targeting, Show Level, Show Title and Organization, Show Health Bar, Show Own Nameplate, Healthbar Style (Level or Health) and Brightness.
  - They are greyed out while Classic is picked, and go back to their defaults when you pick Classic.
  - **Show Own Nameplate** puts a plate over your own character (not in first person), even with Players switched off.

### Misc
- **New page** in Options → Clarity.
- **Hide Alien Smoke Particles** (on by default): hides the glowing smoke clouds under the alien ships at the ICC Shuttleport, ICC Assembly Hall, Serenity Islands, the Superior Organization HQs and elsewhere. They are very heavy on the graphics card, and none of the game's options remove them. Switching it takes effect at once.

## 0.1.15

### Frames (the bars)
- **New look:** your bars and your targets' bars are now in metal frames, and the options page is called **Frames**.
  - Each name is shown in full above its frame, with a dark outline so it reads on bright ground too.
  - Your health and nano show their amounts, like 9,350 / 12,950. The percentages are gone.
  - Target bars show health only, coloured by the target's level in the game's colours, with as many sections as the game's own target bar for that much health. Target levels are no longer shown.
  - The fight target's frame has a pulsing orange ring.
- **Your nano target's target** is back: a small frame with the name of whoever your nano target is targeting.
- **Smoother:** the light piece that showed health just lost is gone, and health changes no longer redraw the whole frame, which made them lag.
- **Game options:** while the frames are on, the game's Show Nano Target, Show Fight Target and Show Target's Target options are greyed out.
- **Fewer options:** the Frames page has just Enabled, Locked and Size. The size now defaults to 80%. The target frames, the target's-target frame and the amounts are always on.

### Nameplates
- **New look:** the name in a frame like the bars', with the org line under it.
  - Only your nano target's and fight target's plates have a health bar.
  - Your nano target gets a white frame, your fight target an orange one, and gold when it's both.
  - A character attacking you gets a blinking red border.
  - The level is no longer shown.
- **Removed:**
  - The Classic style.
  - The plate over your own character. Targeting yourself still shows one.
  - The health bar colour choices. Bars always use the game's colours by level.
  - The background and font settings.
- **Always on now:** clicking a plate to select that character, and showing titles and orgs.
- **Simpler options:** Enabled; Size, Minimum Size, Maximum Size, Opacity and Target Nameplate Size; Overlapping nameplates; and a Nameplates group with Players, NPCs and Monsters, and Pets.

### Options
- Each Clarity page now shows just its name at the top, and the on/off switches are called **Enabled**.
- Only the Floating Combat Text page keeps the Default / Custom choice.

## 0.1.14

### Bars
- **Click to target:** while the bars are locked, click any bar to make that character your nano target. Clicking your own bar targets you.
  - While the bars are unlocked, clicking and dragging still moves them.
- **Your fight target's target:** the small target's-target bar now follows your fight target instead of your nano target. It's hidden while you have no fight target.

### Nameplates
- **Target plates up to 80 m:** the nameplates of your nano target and fight target are hidden once they're more than 80 m away.

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
