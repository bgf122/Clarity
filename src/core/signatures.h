#pragma once

// Byte signatures for non-exported client functions. "??" = wildcard byte.
// Each must match exactly once in its module; tools/verify_sigs.py checks this
// against the DLLs in client/. Format of each line is parsed by that script:
//   #define SIG_<NAME> "<module>", "<pattern>"

// Gamecode.dll: void __stdcall CreateFloatingText(n3Dynel_t*, const char* text, int colorIdx)
// CreateEffect2(0x2F5A, dynel) -> SetColor(colorTable[idx]) -> SetText(text).
// Client VA 0x10011108.
#define SIG_FCT_CREATE "Gamecode.dll", "55 8B EC 56 57 E8 ?? ?? ?? ?? 6A 00 FF 75 08 8B F0 68 5A 2F 00 00 8B CE E8"

// Gamecode.dll: void __thiscall _GfxControlFont_t::Process() (vtable slot 1)
// Client VA 0x100DEB04.
#define SIG_FONT_PROCESS "Gamecode.dll", "55 8B EC 83 EC 28 56 8B F1 E8 ?? ?? ?? ?? 80 7E 14 00 75 72 83 7E 44 00 74 6C D9 EE 8B 4E 34"

// Gamecode.dll: void __thiscall _GfxControlFont_t::SetText(const char*) (vtable slot 18, ret 4)
// Client VA 0x100DEE29.
#define SIG_FONT_SETTEXT "Gamecode.dll", "B8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 EC 24 D9 EE 53 56 57 8B D9 51 8D 4B 74 D9 1C 24 E8"

// Gamecode.dll: inside the combat-message switch (client 0x10014D21):
//   if (dynel->isClientChar && !g_3rdPersonCamera) SendDamageText2D(...)   // GUI path, draws at y=-20
//   else CreateFloatingText(dynel, buf, colorIdx)
// The "75 18" (jne over the 2D call) at offset +16 is patched to "EB 18" so damage on yourself
// in first person also gets floating text.
#define SIG_SELF_DAMAGE_ROUTE "Gamecode.dll", "80 BF 40 01 00 00 00 74 21 80 3D ?? ?? ?? ?? 00 75 18 A1 ?? ?? ?? ?? FF 75 F0"

// Gamecode.dll: void __thiscall CharacterAction handler(int action, int* p20, int chatGroup, int* p28,
// std::string*) (ret 0x14), called by CharacterActionIIR_t::Apply. Action 0xD0 = drain: prints
// "You drained %d points of health/nano from the target." when p20[1] is the client's instance,
// p20[0] = amount, p28[0] = stat (0x1B health), then sets the stat directly - so no floating heal.
// Client VA 0x1005D0D8.
#define SIG_CHAR_ACTION "Gamecode.dll", "B8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 81 EC 90 04 00 00 53 56 57 8B D9 68 ?? ?? ?? ?? 8D 4D B4 FF 15 ?? ?? ?? ??"

// GUI.dll: nametag Indicator builder, void __thiscall (Indicator* ecx), no args. Renders the
// name/org text into a SpriteInfo_t and makes the VisualSprite_t at Indicator+0x18.
// Client VA 0x10024E14.
#define SIG_NAMETAG_BUILD "GUI.dll", "B8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 81 EC E0 00 00 00 53 56 57 8B F9 8B 77 18"

// GUI.dll: nametag Indicator per-frame update, int (Indicator* ecx); 0 = delete the tag.
// Positions the sprite at N3Msg_GetIndicatorPosition.
// Client VA 0x10024D5B.
#define SIG_NAMETAG_UPDATE "GUI.dll", "55 8B EC 83 EC 24 D9 EE 83 65 F4 00 83 65 F8 00 D9 55 E8 53 D9 55 EC 56"
