#pragma once

// In-game "Clarity" tab in the options window, next to AOReloaded's.
// - Each setting is a DistributedValue ("CLR_..."), registered with a range so OptionSlider works.
// - The options window reads OptionPanel/Root.xml through msvcr100's CreateFileA every time it
//   opens; we chain on that import slot (after AOReloaded's own patch) and append our tab.
// - Changes arrive through a DistributedValue_c observer and are written to Clarity.ini, which the
//   config poll then reloads.
namespace prk::settings {

bool Install();

// Called periodically from the init thread: writes changed values to Clarity.ini.
void Flush();

} // namespace prk::settings
