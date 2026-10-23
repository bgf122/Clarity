#include "threat.h"

#include <unordered_map>

namespace prk::threat {

namespace {

struct Attacks {
    DWORD first = 0; // start of the current run of attacks
    DWORD last = 0;
};
std::unordered_map<uint64_t, Attacks> g_attackers;

uint64_t Key(const game::Identity& id)
{
    return (static_cast<uint64_t>(id.type) << 32) | id.instance;
}

} // namespace

void NoteAttack(const game::Identity& attacker)
{
    const DWORD now = GetTickCount();
    Attacks& a = g_attackers[Key(attacker)];
    if (!a.last || now - a.last > kMemoryMs)
        a.first = now; // a new run of attacks
    a.last = now;
    if (g_attackers.size() > 256) {
        for (auto it = g_attackers.begin(); it != g_attackers.end();)
            it = now - it->second.last > kMemoryMs ? g_attackers.erase(it) : std::next(it);
    }
}

bool IsAttackingMe(const game::Identity& id, DWORD* since)
{
    auto it = g_attackers.find(Key(id));
    if (it == g_attackers.end() || GetTickCount() - it->second.last > kMemoryMs)
        return false;
    if (since)
        *since = it->second.first;
    return true;
}

} // namespace prk::threat
