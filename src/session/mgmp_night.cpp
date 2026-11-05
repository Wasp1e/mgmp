#include "mgmp_night.h"
#include "mgmp_net.h"
#include "mgmp_mem.h"
#include "mgmp_log.h"
#include "mgmp_resolve.h"
#include "mgmp_leave.h"
#include "MinHook.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <cwctype>

namespace mgmp {
namespace {

// glaiel::InterstitialScene::init(this, Interstitial, std::function, std::string),
// the Interstitial by pointer. One call per event of the night, from
// DoInterstitialsInternal. The Interstitial, built by sub_1401E3240 (0xF0
// bytes) and filled by the night resolver sub_1401E8B40:
//   +0x60  std::wstring  the event's opening, "{Catname1} ... {Catname2}..."
//   +0x80  std::wstring  the whole of it, the opening, a newline and how it
//                        ended ("...and made a kitten!"); empty for a one-liner
//   +0xA8  std::vector<uint64_t>  the cats it names, by registry key (+0xC48)
// {injury}, {disorder} and {statchanges} are filled in by the resolver; the cat
// names are filled in by the scene as it shows them, so here too.
constexpr uintptr_t kRva_InterInit = 0x20C140;
const uint8_t kInterInitPrologue[8] = { 0x48, 0x8B, 0xC4, 0x4C, 0x89, 0x48, 0x20, 0x4C };
constexpr uintptr_t kInter_Line    = 0x60;
constexpr uintptr_t kInter_Outcome = 0x80;
constexpr uintptr_t kInter_Cats    = 0xA8;

constexpr uintptr_t kRva_CatLookup = 0xD7220;   // (registry, key) -> CatData*
constexpr uintptr_t kDir_Registry  = 0x598;
constexpr uintptr_t kCat_Name      = 0x18;      // std::wstring
// 0 male, 1 female, anything else neither: sub_1400BC230 picks the
// [img:male] / [img:female] / [img:neutral] icon by it.
constexpr uintptr_t kCat_Sex       = 0x5C;

// THE END OF THE DAY IS THE HOST'S. The House's end-day sign runs the
// std::function House::init gives it, <lambda_1>, whose _Do_call is slot 2 of
// vftable 0xEFC860 (sub_1401F8EA0). On the client it ran the client's own night,
// different from the host's, until the next save came (2026-11-05: "visual
// desyncs"). On the client, with a host there, it does nothing.
constexpr uintptr_t kRva_EndDaySlot = 0xEFC860 + 2 * 8;
constexpr uintptr_t kRva_EndDay     = 0x1F8EA0;

constexpr uint32_t kMaxLines   = 32;
constexpr uint64_t kNewNightMs = 20000;   // a gap this long starts another night
constexpr uint64_t kQuietMs    = 3000;    // the host has stopped clicking through
constexpr uint64_t kNoReloadMs = 30000;   // shown anyway if no reload came

using fn_init   = void* (__fastcall*)(void*, void*, void*, void*);
using fn_call   = void  (__fastcall*)(void*);
using fn_lookup = void* (__fastcall*)(void*, uint64_t);

struct State {
    uintptr_t base   = 0;
    fn_init   o_init = nullptr;
    fn_lookup lookup = nullptr;
    fn_call   o_endday = nullptr;
    uint64_t  denied_at = 0;

    // client
    std::string lines[kMaxLines];
    const char* view[kMaxLines] = {};
    uint32_t    count    = 0;
    uint64_t    last_at  = 0;
    bool        reloaded = false;   // the loading screen came up after the lines
    bool        shown    = false;
};
State g;

bool read_ws(const void* ws, std::wstring& out) {
    out.clear();
    uint64_t size = 0, room = 0;
    if (!mem_read((const uint8_t*)ws + 0x10, &size, sizeof(size)) ||
        !mem_read((const uint8_t*)ws + 0x18, &room, sizeof(room)) || size > 4096)
        return false;
    const void* data = ws;
    if (room > 7 && !mem_read(ws, &data, sizeof(data))) return false;
    if (!size) return true;
    out.resize((size_t)size);
    return data && mem_read(data, &out[0], (size_t)size * sizeof(wchar_t));
}

std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

void* safe_lookup(void* reg, uint64_t key) {
    __try { return g.lookup(reg, key); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

void* cat_of(uint64_t key) {
    void* const* slot = (void* const*)addr_of_data(D_MewDirectorPtr);
    void* dir = nullptr;
    void* reg = nullptr;
    if (!g.lookup || !slot || !mem_read(slot, &dir, sizeof(dir)) || !dir ||
        !mem_read((const uint8_t*)dir + kDir_Registry, &reg, sizeof(reg)) || !reg)
        return nullptr;
    return safe_lookup(reg, key);
}

// The sign the game draws beside a name.
const wchar_t* sex_sign(uint64_t key) {
    void* cat = cat_of(key);
    int32_t sex = -1;
    if (!cat || !mem_read((const uint8_t*)cat + kCat_Sex, &sex, sizeof(sex))) return L"";
    return sex == 0 ? L"\u2642" : sex == 1 ? L"\u2640" : L"";
}

std::wstring cat_name(uint64_t key) {
    void* cat = cat_of(key);
    std::wstring name;
    if (!cat || !read_ws((const uint8_t*)cat + kCat_Name, name) || name.empty()) return L"?";
    return name;
}

void replace_all(std::wstring& s, const std::wstring& from, const std::wstring& to) {
    for (size_t at = s.find(from); at != std::wstring::npos; at = s.find(from, at + to.size()))
        s.replace(at, from.size(), to);
}

// The game's markup, [m:happy] and [s:.7]...[/s], means nothing in the window --
// except a stat's icon ("+1 [...str...]" from a fight), which becomes its name.
const wchar_t* stat_word(const std::wstring& tag) {
    static const struct { const wchar_t* id; const wchar_t* ru; } k[] = {
        { L"str", L"\u0421\u0418\u041b" }, { L"dex", L"\u041b\u041e\u0412" },
        { L"con", L"\u0422\u0415\u041b" }, { L"int", L"\u0418\u041d\u0422" },
        { L"spd", L"\u0421\u041a\u0420" }, { L"cha", L"\u0425\u0410\u0420" },
        { L"lck", L"\u0423\u0414\u0427" }, { L"luck", L"\u0423\u0414\u0427" },
    };
    std::wstring t = tag;
    for (wchar_t& c : t) c = (wchar_t)towlower(c);
    for (const auto& e : k) {
        const size_t at = t.find(e.id);
        if (at == std::wstring::npos) continue;
        const size_t len = wcslen(e.id);
        const bool left  = at == 0 || !iswalpha(t[at - 1]);
        const bool right = at + len >= t.size() || !iswalpha(t[at + len]);
        if (left && right) return e.ru;
    }
    return nullptr;
}

void strip_tags(std::wstring& s) {
    std::wstring out, tag;
    bool in = false;
    for (wchar_t c : s) {
        if (c == L'[') { in = true; tag.clear(); continue; }
        if (c == L']' && in) {
            in = false;
            // The resolver has already put the sex in as an icon (sub_1400B8A80
            // fills {gendericonN} with [img:male] / [img:female] / [img:neutral]).
            if (tag == L"img:male")        out += L"\u2642";
            else if (tag == L"img:female") out += L"\u2640";
            else if (const wchar_t* w = stat_word(tag)) out += w;
            continue;
        }
        if (in) tag += c;
        else    out += c;
    }
    s.swap(out);
    replace_all(s, L"  ", L" ");
    replace_all(s, L" !", L"!");
    replace_all(s, L"\n ", L"\n");
    while (!s.empty() && (s.front() == L' ' || s.front() == L'\n')) s.erase(0, 1);
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\n')) s.pop_back();
}

std::string event_text(const void* inter) {
    std::wstring line, whole;
    read_ws((const uint8_t*)inter + kInter_Line, line);
    read_ws((const uint8_t*)inter + kInter_Outcome, whole);
    if (!whole.empty()) line.swap(whole);

    uint64_t* begin = nullptr;
    uint64_t* end   = nullptr;
    std::wstring names[4];
    std::wstring signs[4];
    uint32_t n = 0;
    if (mem_read((const uint8_t*)inter + kInter_Cats, &begin, sizeof(begin)) &&
        mem_read((const uint8_t*)inter + kInter_Cats + 8, &end, sizeof(end)) && begin &&
        end >= begin) {
        for (uint64_t* p = begin; p < end && n < 4; ++p) {
            uint64_t key = 0;
            if (!mem_read(p, &key, sizeof(key))) break;
            signs[n]   = sex_sign(key);
            names[n++] = cat_name(key);
        }
    }
    for (uint32_t i = 0; i < 4; ++i) {
        wchar_t tag[24];
        _snwprintf_s(tag, _TRUNCATE, L"{Catname%u}", i + 1);
        replace_all(line, tag, i < n ? names[i] : L"?");
        _snwprintf_s(tag, _TRUNCATE, L"{gendericon%u}", i + 1);
        replace_all(line, tag, i < n ? signs[i] : L"");
    }
    replace_all(line, L"{Catname's}", n ? names[0] : L"?");
    replace_all(line, L"{Catname}", n ? names[0] : L"?");
    replace_all(line, L"{gendericon}", n ? signs[0] : L"");
    strip_tags(line);
    return utf8(line);
}

void* __fastcall inter_init(void* scene, void* inter, void* fn, void* str) {
    if (inter && net_active() && net_role() == NetRole::Host && net_peer_count() >= 2 &&
        leave_on_house()) {
        const std::string text = event_text(inter);
        if (!text.empty()) {
            LogFileMsg m{};
            m.size = (uint32_t)text.size();
            m.data = (uint8_t*)text.data();
            strncpy_s(m.reason, "night", _TRUNCATE);
            std::string one = text;
            for (char& c : one) if (c == '\n') c = '/';
            if (net_send_night(m)) log_line("NIGHT", "-> %s", one.c_str());
        }
    }
    return g.o_init(scene, inter, fn, str);
}

void __fastcall end_day(void* closure) {
    if (net_active() && net_role() != NetRole::Host && net_peer_count() >= 2) {
        g.denied_at = GetTickCount64();
        log_line("NIGHT", "the end-day sign was pressed here -- the day ends on the host's side, so nothing");
        return;
    }
    g.o_endday(closure);
}

void patch_end_day(uintptr_t base) {
    void** slot = (void**)(base + kRva_EndDaySlot);
    void*  have = nullptr;
    DWORD  prot = 0;
    if (!mem_read(slot, &have, sizeof(have)) || (uintptr_t)have != base + kRva_EndDay ||
        !VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &prot)) {
        log_line("NIGHT", "!! the end-day sign moved -- the other player can still end the day");
        return;
    }
    g.o_endday = (fn_call)have;
    *slot = (void*)&end_day;
    VirtualProtect(slot, sizeof(void*), prot, &prot);
}

} // namespace

bool night_end_day_refused() { return g.denied_at && GetTickCount64() - g.denied_at < 2500; }

void night_set_base(uintptr_t base) {
    if (g.o_init) return;
    g.base = base;
    patch_end_day(base);
    if (memcmp((const void*)(base + kRva_CatLookup), "\x48\x89\x5C\x24\x08\x48\x89\x74", 8) == 0)
        g.lookup = (fn_lookup)(base + kRva_CatLookup);
    void* at = (void*)(base + kRva_InterInit);
    if (memcmp(at, kInterInitPrologue, sizeof(kInterInitPrologue)) != 0 ||
        MH_CreateHook(at, (void*)&inter_init, (void**)&g.o_init) != MH_OK ||
        MH_EnableHook(at) != MH_OK) {
        g.o_init = nullptr;
        log_line("NIGHT", "!! the night's event screen moved -- the other player will not hear about the night");
    }
}

void night_on_message(const LogFileMsg& m) {
    if (net_role() == NetRole::Host || !m.data || !m.size) return;
    const uint64_t now = GetTickCount64();
    // Lines that come while the window is up join it: the host is still
    // clicking through the same night.
    if (now - g.last_at > kNewNightMs) {
        g.count    = 0;
        g.reloaded = false;
        g.shown    = false;
    }
    g.last_at = now;
    if (g.count >= kMaxLines) return;
    g.lines[g.count].assign((const char*)m.data, m.size);
    std::string one = g.lines[g.count];
    for (char& c : one) if (c == '\n') c = '/';
    log_line("NIGHT", "<- %s", one.c_str());
    ++g.count;
}

// At once, from the first event, and over the reload's loading screen: waiting
// for the reload and for the host to stop clicking made it come 15 s after the
// day ended (2026-11-05, the user asked why so slow). Later events join it.
const char* const* night_view(uint32_t* n) {
    *n = 0;
    if (!g.count) return nullptr;
    if (!g.shown) {
        g.shown = true;
        log_line("NIGHT", "the night's window is up (%u event(s) so far)", g.count);
    }
    for (uint32_t i = 0; i < g.count; ++i) g.view[i] = g.lines[i].c_str();
    *n = g.count;
    return g.view;
}

void night_dismiss() {
    g.count = 0;
    g.shown = false;
}

} // namespace mgmp
