// The whole interface is one 2D sprite of DisplaySystem's sprite system: WindowController_c (GUI.dll),
// whose RenderSpriteBase_t part is in SpriteRenderSystem_t's list (a std::list of RenderSpriteBase_t*).
// Each frame DisplaySystem (0x1007BDBA) calls vtable slot 1, Render(viewport, alpha), on every entry -
// for the window controller that draws all windows (and with ao-vk, its interface layer is only shown
// from inside that call). Hiding swaps the window controller's entry for a stand-in whose Render does
// nothing; showing swaps it back. No code is patched. While hidden, the window controller's per-frame
// work (which window is under the mouse, tooltips) waits too; the mouse pointer is a sprite of its own
// and stays.
#include "hideui.h"

#include "../../core/config.h"
#include "../../core/log.h"
#include "../../core/wndproc.h"

#include <cstdint>

namespace prk::hideui {

namespace {

using InstanceFn = void*(__cdecl*)();
InstanceFn g_spriteSystem = nullptr;     // SpriteRenderSystem_t::GetInstance
InstanceFn g_windowController = nullptr; // WindowController_c::GetInstanceIfAny
void* g_wcSpriteVtable = nullptr;        // WindowController_c's RenderSpriteBase_t vtable

struct SpriteNode {
    SpriteNode* next;
    SpriteNode* prev;
    void* value; // RenderSpriteBase_t*
};

// The stand-in: only slot 1 (Render) is called through the list.
void __fastcall NoRender(void* /*self*/, void* /*edx*/, void* /*viewport*/, float /*alpha*/) {}
void* g_standInVtable[2] = {nullptr, reinterpret_cast<void*>(&NoRender)};
struct StandIn {
    void** vtable = g_standInVtable;
} g_standIn;

bool g_hidden = false;
void* g_hiddenWc = nullptr;     // the window controller hidden ...
void* g_hiddenSprite = nullptr; // ... and its RenderSpriteBase_t part

SpriteNode* FindNode(const void* value)
{
    void* system = g_spriteSystem();
    if (!system)
        return nullptr;
    const auto* list = *static_cast<uint32_t* const*>(system); // {head node, size}
    if (!list)
        return nullptr;
    auto* head = reinterpret_cast<SpriteNode*>(list[0]);
    uint32_t left = list[1];
    for (SpriteNode* node = head ? head->next : nullptr; node && node != head && left; node = node->next, --left) {
        if (node->value == value)
            return node;
    }
    return nullptr;
}

// The window controller's RenderSpriteBase_t part: where its vtable pointer is.
void* WcSprite(void* wc)
{
    auto* p = static_cast<void**>(wc);
    for (int i = 0; i < 0x100; ++i) {
        if (p[i] == g_wcSpriteVtable)
            return &p[i];
    }
    return nullptr;
}

bool KeyMatches(const Hotkey& k, UINT msg, WPARAM wp, LPARAM lp)
{
    if (!k.vk || (msg != WM_KEYDOWN && msg != WM_SYSKEYDOWN) || wp != k.vk || (lp & (1 << 30)))
        return false; // (bit 30: auto-repeat)
    const bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0, alt = GetKeyState(VK_MENU) < 0;
    return ctrl == k.ctrl && shift == k.shift && alt == k.alt;
}

void Hide()
{
    void* wc = g_windowController();
    void* sprite = wc ? WcSprite(wc) : nullptr;
    SpriteNode* node = sprite ? FindNode(sprite) : nullptr;
    if (!node) {
        Log("[ui] interface not found in the sprite list - not hidden");
        return;
    }
    node->value = &g_standIn;
    g_hidden = true;
    g_hiddenWc = wc;
    g_hiddenSprite = sprite;
    Log("[ui] interface hidden");
}

void Show()
{
    g_hidden = false;
    SpriteNode* node = FindNode(&g_standIn);
    if (!node) {
        Log("[ui] interface shown (its entry was replaced meanwhile)");
        return;
    }
    // The window controller may have been rebuilt meanwhile (its own entry added anew); then the
    // stand-in stays, drawing nothing, rather than hand back a pointer to a deleted object.
    if (g_windowController() == g_hiddenWc && WcSprite(g_hiddenWc) == g_hiddenSprite && !FindNode(g_hiddenSprite))
        node->value = g_hiddenSprite;
    Log("[ui] interface shown");
}

// The game window's messages: the key toggles, and still reaches the game.
bool OnMessage(HWND /*hwnd*/, UINT msg, WPARAM wp, LPARAM lp, LRESULT* /*result*/)
{
    if (!KeyMatches(Cfg().ui.hideUi, msg, wp, lp))
        return false;
    if (g_hidden)
        Show();
    else
        Hide();
    return false;
}

} // namespace

bool Install()
{
    HMODULE ds = GetModuleHandleA("DisplaySystem.dll");
    HMODULE gui = GetModuleHandleA("GUI.dll");
    if (ds && gui) {
        g_spriteSystem = reinterpret_cast<InstanceFn>(GetProcAddress(ds, "?GetInstance@SpriteRenderSystem_t@@SAPAV1@XZ"));
        g_windowController = reinterpret_cast<InstanceFn>(GetProcAddress(gui, "?GetInstanceIfAny@WindowController_c@@SAPAV1@XZ"));
        g_wcSpriteVtable = reinterpret_cast<void*>(GetProcAddress(gui, "??_7WindowController_c@@6BRenderSpriteBase_t@@@"));
    }
    const bool ok = g_spriteSystem && g_windowController && g_wcSpriteVtable;
    if (ok)
        wndproc::Add(&OnMessage);
    Log("[ui] hide-interface key %s", ok ? "ready" : "unavailable: exports missing");
    return ok;
}

} // namespace prk::hideui
