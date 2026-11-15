#pragma once

// The game's GUI (GUI.dll exports) for features that show painted windows: a plain Window -> View ->
// ViewSurface_c made from a SpriteInfo_t (DisplaySystem) Clarity paints, as the frames do (see bars.cpp
// for how it was worked out), plus helpers for the GUI's views and window list. Game thread only.
#include "canvas.h"

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace prk::gui {

// Rect and Point (Utils.dll) are plain floats; a Rect's right and bottom are inclusive.
struct Rect {
    float left, top, right, bottom;
};
struct Point {
    float x, y;
};

// Window flags (Window ctor): 0x200 / 0x100 keep the window behind / in front of the other windows,
// 1 lets the mouse through it (WindowController_c::HandleMouseMoved skips such windows).
constexpr unsigned kWindowFlags = 0xE3C; // as CharBarWindow_c: behind the other windows
constexpr unsigned kWindowBack = 0x200, kWindowFront = 0x100, kWindowNoMouse = 1;
constexpr unsigned kNormalFlags = kWindowFlags & ~kWindowBack;

// View flags (View +0xAC -> +0x19C): 0x100 = a hidden view gives up its room in its parent's layout.
constexpr unsigned kViewCollapse = 0x100;
constexpr int kMaxNesting = 24;

bool Init(const char* who); // logs missing exports under `who`

void* WindowController(); // WindowController_c::GetInstanceIfAny
bool InGame();            // the control centre made the GUI and there is a client character

template <typename F>
void ForEachWindow(void* wc, F&& f);
bool InWindowList(void* wc, void* window);
bool IsClass(void* obj, const char* decoratedName); // MSVC RTTI, e.g. ".?AVShortcutBarWindow_c@@"
const char* ClassName(void* obj);                    // its MSVC RTTI name (".?AV...@@"), "?" if unknown

// GUI units per client pixel, and the screen's size in GUI units.
float GuiPerPx(HWND hwnd, Point* screen);
void* MouseWindow(void* wc); // the window under the mouse (WindowController_c::GetMouseWindow)
// A game window Clarity hides (the compass, the game's bars) still stops the GUI's tooltip search where it is:
// WindowController_c::UpdateToolTip takes the first window whose frame holds the mouse, shown or not, and looks
// no further. So while hidden it is also made mouse-transparent (Window flag 1, at Window +0x70 -> +0x3C;
// GetFlags 0x101541B6), and given its flags back when shown. Needs no Init (works for any feature).
void MouseThrough(void* window, bool on);
// A window's flags (Window +0x70 -> +0x3C, as Window::GetFlags), to read and change in place; null if none.
unsigned* WindowFlagsField(void* window);
constexpr unsigned kWindowNoRaise = 0x400; // WindowController_c::HandleMouseDown doesn't MoveToFront a window with it
bool Dragging(void* wc);     // something is being dragged (WindowController_c::GetDragObject)

// Windows.
Rect WindowFrame(void* window); // its frame on the screen (GUI units)
void WindowMoveTo(void* window, float x, float y);
bool WindowVisible(void* window);
void WindowFront(void* window);
void WindowSetShown(void* window, bool show); // Window::Show
void* WindowRoot(void* window); // the window's root view (its WndBorder)
// Whether window a is above window b on the screen (both in the window controller's list).
bool Above(void* wc, void* a, void* b);

// Views.
void ViewShow(void* view, bool show); // View::Show(show, absolute)
unsigned ViewFlags(void* view);
void ViewSetFlags(void* view, unsigned flags);
Point ViewToScreen(void* view, Point local); // View::ConvertToScreen(point, true)
Point ViewSize(void* view);                   // View::Width / Height + 1 (its frame's right / bottom are inclusive)
int ViewDepth(void* view);                    // how deep it is in its window: its window's root view is 0

// Child views and game images (for windows that show the game's own icons in their cells).
void* MakeChildView(void* parent, const Rect& frame); // a plain View in `parent`, at `frame` (parent coords)
void SetViewFrame(void* view, const Rect& frame);
void SetViewAlpha(void* view, float alpha);           // View::SetAlpha: the view and everything in it
// ViewSurface_c::Load: an item / nano icon (an RDB icon id: flag 0x80, the icons' own cache) or, with
// icon = false, a GUI skin image (GuiResourceManager id). A skin id that doesn't exist crashes the game.
void* LoadImage(int id, bool icon);
// Shown in the view at dst (view coords). leftHalf: only the image's left half, when it's twice as wide as
// high - a nano's icon is two side by side (the left one normal, the right one dimmed: the NCU window cuts
// between them to show the time left, ItemIconView_c GUI 0x1003EFE6).
void AttachImage(void* view, void* image, const Rect& dst, bool leftHalf = false);
void DestroyImage(void* view, void* image);           // detached and deleted
Rect ImageSource(void* image);                        // the part of the texture it shows (its source frame)
void SetImageFrames(void* image, const Rect& src, const Rect& dst); // which part, and where (view coords)
void SetImageAlpha(void* image, float alpha);         // ViewSurface_c::SetAlpha
// The view's tooltip (HTML, as the game's: <br>, <font color="...">), shown on hover.
void SetToolTip(void* view, const std::string& title, const std::string& text);
// The GUI reads the tooltip under the mouse only on mouse events (WindowController_c::UpdateToolTip, from
// HandleMouseMoved etc.): after changing the text of the one shown, this has it read again; Render swaps
// the shown tooltip for the new text 500 ms later (the old one stays until then).
void RefreshToolTip();

// The colours across a skin image (GuiResourceManager_t::GetGuiTexture id), each the average of its
// visible pixels along the image's length (an upright image is read across its columns): a bar's fill
// shading top to bottom. Empty if it can't be read.
std::vector<uint32_t> SkinProfile(int id);
// A skin image's colour: the average of its visible pixels, the more colourful ones counting more (a tinted
// overlay's tint). `fallback` if it can't be read.
uint32_t SkinColour(int id, uint32_t fallback);

// The game's info window on a URL, as its own links and menus open it: "itemid://<type>/<instance>" shows
// an item's or a nano's description (InfoViewModule_c::ShowURL). False if the game's function is missing.
bool ShowInfoUrl(const std::string& url);

// A painted window: a texture the size of the content (at most texW x texH), shown 1:1.
//
// Depth: the GUI draws with a depth buffer (ViewSurface_c::_CommitRendering: Z test LESS, Z write on, alpha
// test > 0), every top-level window from the same depth (WindowController_c::Render calls each window's
// _CallRender with key 0), a view's surfaces a little nearer for each level it's nested. Two windows' surfaces
// at the same level are at the same depth: where one is drawn first, the other is not drawn at all (the world
// showed through key labels over the action bar frame). `nesting` puts the texture that many empty views
// deeper, so it's nearer than what it's meant to cover (the game's icons are some 8 levels deep).
struct Surface {
    const char* what;
    int texW, texH;
    unsigned flags = kWindowFlags;
    int nesting = 0; // extra views between the window and the texture's view (at most kMaxNesting)

    void* window = nullptr;
    void* windowVtable = nullptr;
    void* view = nullptr;   // the view the texture is on (the innermost)
    void* chain[kMaxNesting] = {}; // the views between the window and it, outermost first
    void* surface = nullptr;
    void* sprite = nullptr;
    int w = 0, h = 0;      // content, pixels
    float gw = 0, gh = 0;  // content, GUI units
    float x = -1, y = -1;  // top-left, GUI units
    std::string drawn;     // what it shows: repainted when this changes
};

// Pins the window to its content's size again if the GUI changed it: adding child views makes it fit the
// window to their preferred size, which plain views don't have - it shrinks to nothing.
void KeepSize(Surface& s);

// The whole window's opacity (its root view's alpha).
void SetOpacity(Surface& s, float alpha);

// Makes the window (if needed) at w x h pixels, k GUI units each. False if it couldn't be made.
bool Ensure(Surface& s, int w, int h, float k);
// Forgets the window if the GUI deleted it (it's no longer in the list or its memory was reused).
void Check(void* wc, Surface& s);
void Forget(Surface& s);
void MoveTo(Surface& s, float x, float y);
void Show(Surface& s, bool show);
// The canvas (the content's size) into the texture; with `part` (pixels), only that part.
void Upload(Surface& s, const gfx::Canvas& c, const RECT* part = nullptr);

// --- implementation details used by the template ---

struct ListNode {
    ListNode* next;
    ListNode* prev;
    void* value;
};
constexpr int kWcWindowList = 0x88; // WindowController_c: std::list<Window*>

template <typename F>
void ForEachWindow(void* wc, F&& f)
{
    auto* head = *reinterpret_cast<ListNode**>(static_cast<uint8_t*>(wc) + kWcWindowList);
    int guard = 0;
    for (ListNode* n = head ? head->next : nullptr; n && n != head && guard < 4096; n = n->next, ++guard)
        f(n->value);
}

} // namespace prk::gui
