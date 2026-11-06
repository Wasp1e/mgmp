#include "mgmp_ability.h"

#include "mgmp_mem.h"

#include <windows.h>
#include <cstdio>
#include <cstring>

namespace mgmp {
namespace {

// Every read below goes through mem_read: these offsets are recovered, not
// declared, and a wrong one must degrade to "unknown" rather than fault.
const void* read_ptr(const void* base, size_t off) {
    if (!base) return nullptr;
    const void* p = nullptr;
    if (!mem_read((const uint8_t*)base + off, &p, sizeof(p))) return nullptr;
    return p;
}

uint32_t read_u32(const void* base, size_t off) {
    if (!base) return 0;
    uint32_t v = 0;
    if (!mem_read((const uint8_t*)base + off, &v, sizeof(v))) return 0;
    return v;
}

// A count read out of a live object is only as trustworthy as the offset it
// came from. Cap the walk so a garbage count cannot turn into a long scan on
// the hot path; a real character has a handful of spells.
const uint32_t kMaxSpellSlots = 64;

} // namespace

AbilitySlot ability_slot_of(const void* character, const void* ability) {
    AbilitySlot s{ SLOT_NONE, 0 };
    if (!ability) return s;                    // an end-turn: genuinely no ability
    s.kind = SLOT_UNKNOWN;
    if (!character) return s;

    if (read_ptr(character, kCharAbilMove)   == ability) { s.kind = SLOT_MOVE;   return s; }
    if (read_ptr(character, kCharAbilAttack) == ability) { s.kind = SLOT_ATTACK; return s; }
    if (read_ptr(character, kCharAbilBonus)  == ability) { s.kind = SLOT_BONUS;  return s; }

    uint32_t    count = read_u32(character, kCharSpellCount);
    const void* data  = read_ptr(character, kCharSpellData);
    if (!data) return s;
    if (count > kMaxSpellSlots) count = kMaxSpellSlots;

    for (uint32_t i = 0; i < count; ++i) {
        const void* slot = nullptr;
        if (!mem_read((const uint8_t*)data + i * sizeof(void*), &slot, sizeof(slot)))
            break;
        if (slot == ability) {
            s.kind  = SLOT_SPELL;
            s.index = (uint8_t)i;
            return s;
        }
    }
    return s;
}

void* ability_from_slot(const void* character, AbilitySlot slot) {
    if (!character) return nullptr;
    switch (slot.kind) {
    case SLOT_NONE:   return nullptr;
    case SLOT_MOVE:   return (void*)read_ptr(character, kCharAbilMove);
    case SLOT_ATTACK: return (void*)read_ptr(character, kCharAbilAttack);
    case SLOT_BONUS:  return (void*)read_ptr(character, kCharAbilBonus);
    case SLOT_SPELL:  break;
    default:          return nullptr;   // SLOT_UNKNOWN is not reproducible
    }

    uint32_t    count = read_u32(character, kCharSpellCount);
    const void* data  = read_ptr(character, kCharSpellData);
    if (!data || slot.index >= count || count > kMaxSpellSlots) return nullptr;

    void* p = nullptr;
    if (!mem_read((const uint8_t*)data + slot.index * sizeof(void*), &p, sizeof(p)))
        return nullptr;
    return p;
}

bool ability_gon_name(const void* ability, char* out, size_t out_size) {
    if (!out || out_size == 0) return false;
    out[0] = 0;
    if (!ability) return false;

    const void* data = read_ptr(ability, kAbilData);
    if (!data) return false;

    // MSVC std::string: union { char buf[16]; char* ptr; } at +0, size at +16,
    // capacity at +24. Capacity > 15 means the heap form. FindAbility performs
    // exactly this test at (Ability+0x28)+0xA0.
    const uint8_t* str = (const uint8_t*)data + kAbilDataName;
    uint64_t size = 0, cap = 0;
    if (!mem_read(str + 16, &size, sizeof(size))) return false;
    if (!mem_read(str + 24, &cap,  sizeof(cap)))  return false;
    if (size >= out_size || size > 4096) return false;   // not a plausible name

    const void* chars = str;
    if (cap > 15) {
        if (!mem_read(str, &chars, sizeof(chars)) || !chars) return false;
    }
    if (!mem_read(chars, out, (size_t)size)) { out[0] = 0; return false; }
    out[size] = 0;
    return true;
}

namespace {
// Character's list of every usable ability: rebuilds Character+0xE90 (a
// CustomVector<Ability*>) and returns its address.
constexpr uintptr_t kRva_AllAbilities = 0x117FC0;
const uint8_t kAllAbilitiesPrologue[8] = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48 };
using fn_all_abilities = void* (__fastcall*)(void*);

void* all_abilities(void* character) {
    static fn_all_abilities fn = nullptr;
    static int checked = 0;
    if (!checked) {
        checked = 1;
        const uint8_t* p = (const uint8_t*)GetModuleHandleW(nullptr) + kRva_AllAbilities;
        uint8_t got[8] = {};
        if (mem_read(p, got, sizeof(got)) && memcmp(got, kAllAbilitiesPrologue, 8) == 0)
            fn = (fn_all_abilities)p;
    }
    if (!fn || !character) return nullptr;
    __try { return fn(character); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
} // namespace

void* ability_find_by_name(void* character, const char* gon, char* seen, size_t seen_size) {
    if (seen && seen_size) seen[0] = 0;
    if (!character || !gon || !gon[0]) return nullptr;
    char nm[64];
    const size_t fixed[3] = { kCharAbilMove, kCharAbilAttack, kCharAbilBonus };
    for (size_t off : fixed) {
        void* a = (void*)read_ptr(character, off);
        if (a && ability_gon_name(a, nm, sizeof(nm)) && strcmp(nm, gon) == 0) return a;
    }
    uint32_t    count = read_u32(character, kCharSpellCount);
    const void* data  = read_ptr(character, kCharSpellData);
    if (count > kMaxSpellSlots) count = kMaxSpellSlots;
    for (uint32_t i = 0; data && i < count; ++i) {
        void* a = nullptr;
        if (!mem_read((const uint8_t*)data + i * sizeof(void*), &a, sizeof(a))) break;
        if (a && ability_gon_name(a, nm, sizeof(nm)) && strcmp(nm, gon) == 0) return a;
    }
    const void* list = all_abilities(character);
    if (!list) return nullptr;
    uint32_t    n  = read_u32(list, 4);
    const void* el = read_ptr(list, 8);
    if (n > kMaxSpellSlots) n = kMaxSpellSlots;
    size_t at = 0;
    for (uint32_t i = 0; el && i < n; ++i) {
        void* a = nullptr;
        if (!mem_read((const uint8_t*)el + i * sizeof(void*), &a, sizeof(a))) break;
        if (!a || !ability_gon_name(a, nm, sizeof(nm))) continue;
        if (strcmp(nm, gon) == 0) return a;
        if (seen && at + 1 < seen_size)
            at += _snprintf_s(seen + at, seen_size - at, _TRUNCATE, "%s%s", at ? ", " : "", nm);
    }
    return nullptr;
}

void* current_actor(const void* turn_control) {
    if (!turn_control) return nullptr;

    void* actor = (void*)read_ptr(turn_control, kTcCurrentActor);
    if (!actor) return nullptr;

    // The handle redirect GetCurrentActor applies: if Character+0x478 points at
    // something whose preceding qword still matches the generation stored at
    // Character+0x480, that target is the real actor (possession / mount).
    const void* h = read_ptr(actor, kCharHandlePtr);
    if (h) {
        uint64_t stamp = 0, gen = 0;
        if (mem_read((const uint8_t*)h - 8, &stamp, sizeof(stamp)) &&
            mem_read((const uint8_t*)actor + kCharHandleGen, &gen, sizeof(gen)) &&
            stamp == gen)
            return (void*)h;
    }
    return actor;
}

} // namespace mgmp
