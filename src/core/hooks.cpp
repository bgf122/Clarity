#include "hooks.h"

#include "log.h"

#include <windows.h>

#include <MinHook.h>

#include <cstdlib>
#include <vector>

namespace prk {

namespace {

std::vector<int> ParsePattern(const char* text)
{
    std::vector<int> out;
    for (const char* p = text; *p;) {
        if (*p == ' ') {
            ++p;
        } else if (p[0] == '?' && p[1] == '?') {
            out.push_back(-1);
            p += 2;
        } else {
            out.push_back(static_cast<int>(strtoul(p, const_cast<char**>(&p), 16)));
        }
    }
    return out;
}

} // namespace

uint8_t* FindUnique(const char* module, const char* pattern, const char* what)
{
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleA(module));
    if (!base) {
        Log("[sig] %s: module %s not loaded", what, module);
        return nullptr;
    }
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const size_t size = nt->OptionalHeader.SizeOfImage;
    const std::vector<int> pat = ParsePattern(pattern);

    // Only scan executable sections so we never touch unmapped gaps.
    uint8_t* found = nullptr;
    int hits = 0;
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (int s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE))
            continue;
        uint8_t* begin = base + sec->VirtualAddress;
        size_t len = sec->Misc.VirtualSize;
        if (sec->VirtualAddress + len > size || len < pat.size())
            continue;
        for (size_t i = 0; i + pat.size() <= len; ++i) {
            size_t k = 0;
            while (k < pat.size() && (pat[k] < 0 || begin[i + k] == pat[k]))
                ++k;
            if (k == pat.size()) {
                if (++hits == 1)
                    found = begin + i;
            }
        }
    }
    if (hits != 1) {
        Log("[sig] %s: %d matches in %s (expected 1) - client update? feature disabled", what, hits, module);
        return nullptr;
    }
    Log("[sig] %s: %s+0x%X", what, module, static_cast<unsigned>(found - base));
    return found;
}

bool HooksInit()
{
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        Log("[hook] MH_Initialize failed: %s", MH_StatusToString(st));
        return false;
    }
    return true;
}

bool InstallHook(const char* what, void* target, void* detour, void** original)
{
    if (!target)
        return false;
    MH_STATUS st = MH_CreateHook(target, detour, original);
    if (st != MH_OK) {
        Log("[hook] %s: create failed at %p: %s", what, target, MH_StatusToString(st));
        return false;
    }
    st = MH_EnableHook(target);
    if (st != MH_OK) {
        Log("[hook] %s: enable failed at %p: %s", what, target, MH_StatusToString(st));
        MH_RemoveHook(target);
        return false;
    }
    Log("[hook] %s installed at %p, trampoline=%p", what, target, *original);
    return true;
}

} // namespace prk
