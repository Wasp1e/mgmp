#include "mgmp_house.h"
#include "mgmp_house_drag.h"
#include "mgmp_net.h"
#include "mgmp_log.h"
#include "mgmp_savefile.h"
#include "mgmp_night.h"
#include "mgmp_mem.h"
#include "mgmp_leave.h"
#include "MinHook.h"
#include "mgmp_rng.h"
#include "mgmp_resolve.h"
#include "mgmp_addresses.h"
#include "mgmp_invsync.h"
#include "mgmp_lang.h"
#include "mgmp_cutscene.h"
#include "mgmp_config.h"
#include "mgmp_startrun.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <map>
#include <intrin.h>

namespace mgmp {
namespace {

// ---------------------------------------------------------------------------
// A read-only walk of the save's sqlite tables. Only what the .sav uses: rowid
// tables, legacy journal (no WAL), overflow chains for the big blobs.

struct Db {
    const uint8_t* d = nullptr;
    size_t   n    = 0;
    uint32_t page = 0;   // page size
    uint32_t use  = 0;   // usable bytes per page
};

uint32_t be16(const uint8_t* p) { return ((uint32_t)p[0] << 8) | p[1]; }
uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

// A varint at p (at most 9 bytes), bounds-checked against end.
bool varint(const uint8_t*& p, const uint8_t* end, uint64_t& v) {
    v = 0;
    for (int i = 0; i < 9; ++i) {
        if (p >= end) return false;
        const uint8_t b = *p++;
        if (i == 8) { v = (v << 8) | b; return true; }
        v = (v << 7) | (b & 0x7F);
        if (!(b & 0x80)) return true;
    }
    return true;
}

const uint8_t* page_at(const Db& db, uint32_t no) {
    if (no == 0 || (size_t)no * db.page > db.n) return nullptr;
    return db.d + (size_t)(no - 1) * db.page;
}

// A cell's whole payload, the overflow chain followed.
bool payload(const Db& db, const uint8_t* p, const uint8_t* pend, uint64_t size, std::vector<uint8_t>& out) {
    out.clear();
    if (size > (64u << 20)) return false;
    const uint64_t U = db.use, X = U - 35, M = ((U - 12) * 32 / 255) - 23;
    uint64_t local = size;
    if (size > X) {
        const uint64_t K = M + ((size - M) % (U - 4));
        local = K <= X ? K : M;
    }
    if (p + local > pend) return false;
    out.assign(p, p + local);
    if (local == size) return true;
    if (p + local + 4 > pend) return false;
    uint32_t next = be32(p + local);
    for (int guard = 0; next && out.size() < size && guard < 100000; ++guard) {
        const uint8_t* pg = page_at(db, next);
        if (!pg) return false;
        const uint64_t take = (size - out.size() < U - 4) ? size - out.size() : U - 4;
        out.insert(out.end(), pg + 4, pg + 4 + take);
        next = be32(pg);
    }
    return out.size() == size;
}

struct Cell { uint64_t rowid; std::vector<uint8_t> rec; };

// Every row of the table b-tree rooted at `root`.
bool walk(const Db& db, uint32_t root, std::vector<Cell>& rows, int depth = 0) {
    if (depth > 32) return false;
    const uint8_t* pg = page_at(db, root);
    if (!pg) return false;
    const uint8_t* h = pg + (root == 1 ? 100 : 0);
    const uint8_t type = h[0];
    const uint32_t cells = be16(h + 3);
    const uint8_t* ptrs = h + (type == 0x05 ? 12 : 8);
    const uint8_t* pend = pg + db.page;
    if (type == 0x05) {
        for (uint32_t i = 0; i < cells; ++i) {
            const uint32_t off = be16(ptrs + 2 * i);
            if (off + 4 > db.page || !walk(db, be32(pg + off), rows, depth + 1)) return false;
        }
        return walk(db, be32(h + 8), rows, depth + 1);
    }
    if (type != 0x0D) return false;
    for (uint32_t i = 0; i < cells; ++i) {
        const uint32_t off = be16(ptrs + 2 * i);
        if (off >= db.page) return false;
        const uint8_t* p = pg + off;
        uint64_t size = 0, rowid = 0;
        if (!varint(p, pend, size) || !varint(p, pend, rowid)) return false;
        Cell c;
        c.rowid = rowid;
        if (!payload(db, p, pend, size, c.rec)) return false;
        rows.push_back(std::move(c));
    }
    return true;
}

struct Val {
    uint64_t type = 0;         // the serial type
    const uint8_t* p = nullptr;
    uint32_t len = 0;
};

uint32_t serial_len(uint64_t t) {
    static const uint32_t k[] = { 0, 1, 2, 3, 4, 6, 8, 8, 0, 0, 0, 0 };
    if (t < 12) return k[t];
    return (uint32_t)((t - 12) / 2);
}

bool columns(const std::vector<uint8_t>& rec, std::vector<Val>& out) {
    out.clear();
    const uint8_t* p = rec.data();
    const uint8_t* end = p + rec.size();
    uint64_t hsize = 0;
    if (!varint(p, end, hsize) || hsize > rec.size()) return false;
    const uint8_t* hend = rec.data() + hsize;
    const uint8_t* body = hend;
    while (p < hend) {
        Val v;
        if (!varint(p, hend, v.type)) return false;
        v.len = serial_len(v.type);
        v.p = body;
        if (body + v.len > end) return false;
        body += v.len;
        out.push_back(v);
    }
    return true;
}

int64_t int_of(const Val& v) {
    if (v.type == 8) return 0;
    if (v.type == 9) return 1;
    if (v.type < 1 || v.type > 6) return 0;
    int64_t x = (int8_t)v.p[0];
    for (uint32_t i = 1; i < v.len; ++i) x = (x << 8) | v.p[i];
    return x;
}

std::string text_of(const Val& v) {
    if (v.type >= 13 && (v.type & 1)) return std::string((const char*)v.p, v.len);
    if ((v.type >= 1 && v.type <= 6) || v.type == 8 || v.type == 9) return std::to_string(int_of(v));
    if (v.type == 7) {
        uint64_t b = 0;
        for (int i = 0; i < 8; ++i) b = (b << 8) | v.p[i];
        double f; memcpy(&f, &b, 8);
        char s[40]; snprintf(s, sizeof(s), "%.6g", f);
        return s;
    }
    if (v.type >= 12) {   // a blob: a short one in hex, a long one by its size
        if (v.len > 32) return "<" + std::to_string(v.len) + " bytes>";
        std::string s;
        char x[3];
        for (uint32_t i = 0; i < v.len; ++i) { snprintf(x, sizeof(x), "%02x", v.p[i]); s += x; }
        return s;
    }
    return "null";
}

uint64_t fnv(uint64_t h, const void* p, size_t n) {
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 0x100000001B3ull; }
    return h;
}

uint64_t hash_val(const Val& v) {
    uint64_t h = 0xCBF29CE484222325ull;
    h = fnv(h, &v.type, sizeof(v.type));
    return fnv(h, v.p, v.len);
}

// One row of the House: "table key", the hash of its value, and the value
// itself when it is short enough to read in a log.
struct Row {
    std::string id;
    uint64_t    hash = 0;
    std::string shown;
};

const char* const kTables[] = { "cats", "furniture", "files", "properties", "winning_teams" };

bool parse_house(const uint8_t* data, size_t size, std::vector<Row>& rows);

bool read_house(const wchar_t* path, std::vector<Row>& rows, uint64_t* age_ms) {
    rows.clear();
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    FILETIME wt{};
    GetFileSizeEx(f, &sz);
    GetFileTime(f, nullptr, nullptr, &wt);
    std::vector<uint8_t> buf;
    bool ok = sz.QuadPart > 100 && sz.QuadPart < (64ll << 20);
    if (ok) {
        buf.resize((size_t)sz.QuadPart);
        DWORD got = 0;
        ok = ReadFile(f, buf.data(), (DWORD)buf.size(), &got, nullptr) && got == buf.size();
    }
    CloseHandle(f);
    if (!ok) return false;
    if (age_ms) {
        FILETIME now{};
        GetSystemTimeAsFileTime(&now);
        const uint64_t a = ((uint64_t)now.dwHighDateTime << 32) | now.dwLowDateTime;
        const uint64_t b = ((uint64_t)wt.dwHighDateTime << 32) | wt.dwLowDateTime;
        *age_ms = a > b ? (a - b) / 10000 : 0;
    }

    return parse_house(buf.data(), buf.size(), rows);
}

bool parse_house(const uint8_t* data, size_t size, std::vector<Row>& rows) {
    rows.clear();
    if (!data || size < 100) return false;
    Db db;
    db.d = data;
    db.n = size;
    db.page = be16(db.d + 16);
    if (db.page == 1) db.page = 65536;
    if (db.page < 512 || memcmp(db.d, "SQLite format 3", 15) != 0) return false;
    db.use = db.page - db.d[20];

    std::vector<Cell> master;
    if (!walk(db, 1, master)) return false;
    std::vector<Val> cols;
    for (const Cell& m : master) {
        if (!columns(m.rec, cols) || cols.size() < 4 || text_of(cols[0]) != "table") continue;
        const std::string name = text_of(cols[1]);
        bool want = false;
        for (const char* t : kTables) want |= name == t;
        if (!want) continue;
        std::vector<Cell> cells;
        if (!walk(db, (uint32_t)int_of(cols[3]), cells)) {
            log_line("HOUSE", "!! could not read the save's table '%s'", name.c_str());
            continue;
        }
        for (const Cell& c : cells) {
            if (!columns(c.rec, cols) || cols.size() < 2) continue;
            Row r;
            // An INTEGER PRIMARY KEY is the rowid, stored as a null.
            const std::string key = cols[0].type == 0 ? std::to_string(c.rowid) : text_of(cols[0]);
            r.id    = name + " " + key;
            r.hash  = hash_val(cols[1]);
            // A furniture row ends with its tile x, y, its draw order and two
            // ones. The draw order is the order pieces were put down in (two
            // shelves put down the other way round on each screen, 2026-11-15),
            // and says nothing but which one is drawn on top: left out.
            if (name == "furniture" && cols[1].len >= 20 && cols[1].type >= 12) {
                std::vector<uint8_t> b(cols[1].p, cols[1].p + cols[1].len);
                memset(&b[b.size() - 12], 0, 4);
                Val v = cols[1];
                v.p = b.data();
                r.hash = hash_val(v);
            }
            r.shown = name == "properties" ? text_of(cols[1])
                                           : "<" + std::to_string(cols[1].len) + " bytes>";
            rows.push_back(std::move(r));
        }
    }
    return !rows.empty();
}

// Rows that differ on every day and say nothing about the House.
bool ignored(const std::string& id) {
    return id == "properties savefile_timer" || id == "properties savefile_timer_adjusted";
}

// Rows that differ for a known reason, shown but not counted:
//   house_state  each cat's room by name AND its spot in the room (doubles),
//                and the spot is wherever it wandered or was let go (the
//                2026-11-15 test: the same rooms, other spots). The rooms
//                themselves are compared live (live_cat).
//   random_seed  the House's stream; each game's own until the night is the
//                host's.
//   pedigree     see below.
const char* noted(const std::string& id) {
    if (id == "files house_state") return "the cats' spots inside their rooms";
    if (id == "properties random_seed") return "each game's own stream until the night is the host's";
    // A cache of the cats' kinship, filled for the pairs a player looked at
    // (the host's had (3,5), (3,8)... with 1.0s, the friend's had zeros and
    // stray heap bytes in the same slots, 2026-11-15) -- the same size, other
    // bytes. The cats themselves, parents and all, are compared row by row.
    if (id == "files pedigree") return "the kinship cache, filled for the pairs each player looked at";
    // The NPCs' stock and progress: Little Jack's shop sold the host a piece and
    // the friend's still had it (2026-11-15). Nothing the night reads.
    if (id == "files npc_progress") return "the NPCs' stock and progress, not the night's";
    // The weather is drawn in the morning, after the night and off its stream:
    // the friend's came out Windy, the host's clear (2026-11-15). The morning
    // takes the host's; the evening compares it live (live_weather).
    if (id == "properties current_house_weather") return "the weather: the host's is taken in the morning";
    // Written to the save now and then; the purse is compared live (live_purse).
    if (id == "properties house_gold" || id == "properties house_food") return "the purse, compared live";
    return nullptr;
}

// The cat's name, for a `cats` row.
std::string label(const std::string& id) {
    size_t at = 0;
    if (id.compare(0, 5, "cats ") == 0) at = 5;
    else if (id.compare(0, 9, "live_cat ") == 0) at = 9;
    else return id;
    const uint64_t key = _strtoui64(id.c_str() + at, nullptr, 10);
    const std::string name = night_cat_name(key);
    return name.empty() ? id : id + " (" + name + ")";
}

void send(const char* reason, const std::string& text) {
    LogFileMsg m{};
    m.size = (uint32_t)text.size();
    m.data = (uint8_t*)text.data();
    strncpy_s(m.reason, reason, _TRUNCATE);
    if (!net_send_night(m)) log_line("HOUSE", "!! could not send the House (%u bytes)", m.size);
}

bool     g_match    = false;   // client: the last day-end check found the host's House here
uint64_t g_match_at = 0;

// Where each House cat is now, read from the game rather than its save: the
// House writes the save only now and then (268 s old at one day's end in the
// 2026-11-15 test), so moves made since were not in what was compared.
void live_rows(std::vector<Row>& rows);

// The weather string: the progression ([MewDirector]+0x5A8), +0x4F8 (what the
// save writes as current_house_weather, sub_1401D2550).
constexpr uintptr_t kDir_Progression = 0x5A8;
constexpr uintptr_t kProg_Weather    = 0x4F8;
void* weather_str() {
    void* const* slot = (void* const*)addr_of_data(D_MewDirectorPtr);
    void* dir = nullptr;
    void* prog = nullptr;
    if (!slot || !mem_read(slot, &dir, sizeof(dir)) || !dir ||
        !mem_read((const uint8_t*)dir + kDir_Progression, &prog, sizeof(prog)) || !prog)
        return nullptr;
    return (uint8_t*)prog + kProg_Weather;
}
constexpr uintptr_t kRva_FurnitureStore = 0x13DA9A0;

void compare(const std::string& host_text) {
    wchar_t path[MAX_PATH];
    std::vector<Row> mine;
    uint64_t age = 0;
    if (!savefile_live_path(path, MAX_PATH) || !read_house(path, mine, &age)) {
        log_line("HOUSE", "!! could not read this game's save -- the House is not checked");
        send("house_verdict", "the friend could not read its own save");
        return;
    }
    if (invsync_has_pending() && !invsync_apply_pending("the House check"))
        log_line("HOUSE", "!! the host's inventory could not be put in before the check");
    live_rows(mine);
    std::map<std::string, const Row*> here;
    for (const Row& r : mine) here[r.id] = &r;
    // The inventory is compared live (live_inventory) once the host's is in.
    const bool live_inventory = here.count("live_inventory hash") &&
                                host_text.find("live_inventory hash	") != std::string::npos;
    // Both games read their furniture live: the save's rows are not compared.
    const bool live_furniture = here.count("live_furniture count") &&
                                host_text.find("live_furniture count\t") != std::string::npos;

    // "id\thash\tshown\n" per row.
    std::map<std::string, bool> seen;
    uint32_t rows = 0, differ = 0, missing = 0, extra = 0, shown = 0;
    std::map<std::string, uint32_t> by_table;
    size_t at = 0;
    while (at < host_text.size()) {
        size_t nl = host_text.find('\n', at);
        if (nl == std::string::npos) nl = host_text.size();
        const std::string line = host_text.substr(at, nl - at);
        at = nl + 1;
        const size_t t1 = line.find('\t'), t2 = t1 == std::string::npos ? t1 : line.find('\t', t1 + 1);
        if (t2 == std::string::npos) continue;
        const std::string id = line.substr(0, t1);
        const uint64_t hash = _strtoui64(line.substr(t1 + 1, t2 - t1 - 1).c_str(), nullptr, 16);
        const std::string hv = line.substr(t2 + 1);
        ++rows;
        seen[id] = true;
        if (ignored(id)) continue;
        if (live_furniture && id.compare(0, 10, "furniture ") == 0) continue;   // compared live
        if (live_inventory && id.compare(0, 16, "files inventory_") == 0) continue;
        auto it = here.find(id);
        if (const char* why = noted(id)) {
            if (it != here.end() && it->second->hash != hash)
                log_line("HOUSE", "  (not counted: %s) %s differs", why, id.c_str());
            continue;
        }
        if (it == here.end()) {
            ++missing;
            ++by_table[id.substr(0, id.find(' '))];
            if (shown++ < 60) log_line("HOUSE", "  only the host has %s = %s", label(id).c_str(), hv.c_str());
        } else if (it->second->hash != hash) {
            ++differ;
            ++by_table[id.substr(0, id.find(' '))];
            if (shown++ < 60) log_line("HOUSE", "  %s: host %s, here %s", label(id).c_str(), hv.c_str(),
                                       it->second->shown.c_str());
        }
    }
    for (const Row& r : mine) {
        if (seen.count(r.id) || ignored(r.id)) continue;
        if (live_furniture && r.id.compare(0, 10, "furniture ") == 0) continue;
        if (live_inventory && r.id.compare(0, 16, "files inventory_") == 0) continue;
        ++extra;
        ++by_table[r.id.substr(0, r.id.find(' '))];
        if (shown++ < 60) log_line("HOUSE", "  only this game has %s = %s", label(r.id).c_str(), r.shown.c_str());
    }

    char verdict[400];
    if (!differ && !missing && !extra) {
        snprintf(verdict, sizeof(verdict), "the friend's House is the host's (%u rows; its save written %llu ms before)",
                 rows, (unsigned long long)age);
    } else {
        std::string tables;
        for (const auto& kv : by_table) tables += " " + kv.first + "=" + std::to_string(kv.second);
        snprintf(verdict, sizeof(verdict),
                 "the friend's House differs: %u row(s) differ, %u only the host has, %u only the friend has"
                 " (by table:%s; %u rows; its save written %llu ms before)",
                 differ, missing, extra, tables.c_str(), rows, (unsigned long long)age);
    }
    log_line("HOUSE", "%s%s", (differ || missing || extra) ? "!! " : "", verdict);
    send("house_verdict", verdict);
    g_match    = !differ && !missing && !extra;
    g_match_at = GetTickCount64();
}

// ---------------------------------------------------------------------------
// THE CATS' PLACES. A House cat dragged and let go goes through the drop,
// sub_1401FF140(drag): the dragged HouseCat at drag+0x38, its holder
// (HouseCat+0xE8) cleared, the place under the cursor found by sub_1401E6460
// -- a walk of the scene's component list 0x1D2 (comps+0x1D20: {.., u32 count
// at +0xC, ptr at +0x10}), each place's bounds from its vtable slot 28 -- and
// the cat put there with sub_1402E88D0(place, cat), which takes it out of the
// place it was in and sets +0xE8. The places are the rooms
// (CatPlacementArea), the pipe (HousePipe) and Butch's box (ButchBox, synced
// on its own by mgmp_startrun).
//
// Each game keeps its own House, so a cat moved on one screen stayed where it
// was on the other, and the night -- who breeds with whom is by room -- was
// resolved from two different Houses. A drop is sent as the cat's key and the
// place's position in that list, which is built the same way from the same
// save on both; the other game puts the same cat in the same place with the
// same call, from inside a House component's update.
constexpr uintptr_t kRva_Drop       = 0x1FF140;
const uint8_t kDropHead[10]         = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0x41, 0x38 };
constexpr uintptr_t kRva_PlaceAdd   = 0x2E88D0;
const uint8_t kPlaceAddHead[8]      = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74 };
constexpr uintptr_t kRva_RoomVtbl   = 0xEFCF10;   // CatPlacementArea
constexpr uintptr_t kRva_PipeVtbl   = 0xEFC9C0;   // HousePipe
constexpr uintptr_t kRva_BoxVtbl    = 0xEFCCC0;   // ButchBox
constexpr uintptr_t kDrag_Cat       = 0x38;
constexpr uintptr_t kHouseCat_Key   = 0x80;
constexpr uintptr_t kHouseCat_Place = 0xE8;
constexpr uintptr_t kComp_Owner     = 0x18;
constexpr uintptr_t kOwner_Scene    = 0x8;
constexpr uintptr_t kScene_Comps    = 0x20;
constexpr uintptr_t kComps_Places   = 0x1D20;
constexpr uintptr_t kComps_Cats     = 0x4480;
constexpr uint64_t  kMoveSettleMs   = 300;
// OUTSIDE is no place: a cat let go over the yard is in no place's list and
// keeps +0xE8 null (the drop clears it, and finds nothing to put it in), and
// the night counts only the cats in the House. Sent as this index.
constexpr uint32_t  kOutside        = 0xFFFFFFFFu;
constexpr uintptr_t kRva_PlaceRemove = 0x2E89C0;
const uint8_t kPlaceRemoveHead[8]   = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83 };    // after the box's own ops, which take a cat back to its room

using fn_drop  = void (__fastcall*)(void*);
using fn_place = void (__fastcall*)(void*, void*);
uintptr_t g_base     = 0;
fn_drop   o_drop     = nullptr;
fn_place  place_add  = nullptr;
fn_place  place_remove = nullptr;
bool      g_applying = false;

struct Move {
    uint64_t key; uint32_t index, count, kind; uint64_t at;
    uint8_t from = 255;
    uint64_t drag_sequence = 0;
};
constexpr uint32_t kMoveDeliver   = 0xD;     // a pipe hand-over (index = the NPC choice)
constexpr uint64_t kDeliverSettleMs = 900;   // the pipe here takes the cat and opens its map first
CRITICAL_SECTION g_cs;
bool     g_cs_ready = false;
Move     g_moves[32];
uint32_t g_move_n = 0;
// Experimental position samples use a separate message kind. Never send them
// unless explicitly enabled on both peers.
constexpr uint8_t kChoiceHouseWalk = 47;
constexpr uint8_t kChoiceHouseAction = 48;
constexpr uint8_t kChoiceHouseDragTrace = 49;
uint64_t g_walk_pause_until = 0;
struct WalkSample {
    uint64_t key, at;
    uint32_t place, x, y, state;
    int32_t target_x, target_y;
    double timer;
    bool animation_busy;
    uint8_t idle_flag;
};
std::map<uint64_t, WalkSample> g_walk_pending;
struct HouseAction { uint64_t key, at; uint32_t place, state; char name[16]; };
std::map<uint64_t, HouseAction> g_action_pending;
HouseDragTracker g_drag_tracker;
uint64_t g_drag_sequence = 0;
uint64_t g_local_drag_key = 0;
uint64_t g_drag_previous = 0, g_drag_sample_at = 0;
struct DragMessage { ChoiceMsg msg; uint8_t from; };
std::vector<DragMessage> g_drag_messages;
bool drag_ready();
void drag_tick(void* comps);
void drag_install();
bool remote_drag_cat(void* cat);
void finish_remote_drag(void* cat, bool restore);
bool drag_move_allowed(uint64_t key, uint8_t from);
void drag_publish_final(void* cat);
void drag_guard_note(void* cat, bool box);

struct List { uint32_t n = 0; void** data = nullptr; };

bool comps_of(void* component, void*& comps) {
    void* owner = nullptr; void* scene = nullptr;
    comps = nullptr;
    return component &&
           mem_read((const uint8_t*)component + kComp_Owner, &owner, sizeof(owner)) && owner &&
           mem_read((const uint8_t*)owner + kOwner_Scene, &scene, sizeof(scene)) && scene &&
           mem_read((const uint8_t*)scene + kScene_Comps, &comps, sizeof(comps)) && comps;
}

bool list_of(void* comps, uintptr_t at, List& l) {
    void* list = nullptr;
    l = List{};
    return mem_read((const uint8_t*)comps + at, &list, sizeof(list)) && list &&
           mem_read((const uint8_t*)list + 0xC, &l.n, sizeof(l.n)) &&
           mem_read((const uint8_t*)list + 0x10, &l.data, sizeof(l.data)) && l.data && l.n <= 4096;
}

void* item(const List& l, uint32_t i) {
    void* p = nullptr;
    return i < l.n && mem_read(&l.data[i], &p, sizeof(p)) ? p : nullptr;
}

uint32_t kind_of(void* place) {
    uintptr_t vt = 0;
    if (!mem_read(place, &vt, sizeof(vt))) return 0;
    if (vt == g_base + kRva_RoomVtbl) return 1;
    if (vt == g_base + kRva_PipeVtbl) return 2;
    if (vt == g_base + kRva_BoxVtbl)  return 4;
    return 3;
}

const char* kind_name(uint32_t k) {
    return k == 1 ? "room" : k == 2 ? "pipe" : k == 4 ? "box" : "place";
}

bool sharing() {
    return net_active() && net_peer_count() >= 2 && leave_on_house();
}

// The game's component lookup at 0xECF30 reads component 0x449; its +0x38
// points to the cat currently in this player's hand. Read-only diagnostics to
// validate pickup, continuous movement and release before replicating them.
void trace_house_drag(void* comps) {
    uint64_t& previous = g_drag_previous;
    uint64_t& sample_at = g_drag_sample_at;
    if ((!config().house_positions && !config().house_drag_trace && !config().house_drag_sync) || !sharing()) {
        previous = 0;
        g_local_drag_key = 0;
        return;
    }
    List drags;
    if (!list_of(comps, 0x4490, drags)) return;
    void* drag = item(drags, 0);
    void* cat = nullptr;
    if (!drag || !mem_read((const uint8_t*)drag + kDrag_Cat, &cat, sizeof(cat))) return;
    uint64_t key = 0;
    if (cat && !mem_read((const uint8_t*)cat + kHouseCat_Key, &key, sizeof(key))) return;
    const uint64_t now = GetTickCount64();
    if (previous && previous != key) {
        log_line("HOUSEDRAG", "released cat %016llx", (unsigned long long)previous);
        if (config().house_drag_trace || drag_ready()) {
            ChoiceMsg m{};
            m.kind = kChoiceHouseDragTrace; m.cat = previous; m.index = 0;
            m.node_seed = ++g_drag_sequence;
            net_send_choice(m);
        }
    }
    if (key && previous != key) {
        log_line("HOUSEDRAG", "picked up cat %016llx", (unsigned long long)key);
        sample_at = 0;
    }
    previous = key;
    g_local_drag_key = key;
    g_drag_tracker.exclude_local(key, net_self());
    if (!cat || !key || now - sample_at < (drag_ready() ? 50u : 200u)) return;
    sample_at = now;
    void* transform = nullptr;
    void* holder = nullptr;
    double x = 0, y = 0;
    if (mem_read((const uint8_t*)cat + kHouseCat_Place, &holder, sizeof(holder)) &&
        mem_read((const uint8_t*)cat + 0x40, &transform, sizeof(transform)) && transform &&
        mem_read((const uint8_t*)transform + 0x80, &x, sizeof(x)) &&
        mem_read((const uint8_t*)transform + 0x88, &y, sizeof(y))) {
        log_line("HOUSEDRAG", "holding cat %016llx x=%.3f y=%.3f holder=%s",
                 (unsigned long long)key, x, y, holder ? kind_name(kind_of(holder)) : "none");
        if ((config().house_drag_trace || drag_ready()) && std::isfinite(x) && std::isfinite(y) &&
            std::abs(x) <= 10000 && std::abs(y) <= 10000) {
            const float fx = (float)x, fy = (float)y;
            ChoiceMsg m{};
            m.kind = kChoiceHouseDragTrace; m.cat = key; m.index = 1;
            m.node_seed = ++g_drag_sequence;
            memcpy(&m.count, &fx, sizeof(fx)); memcpy(&m.aux, &fy, sizeof(fy));
            net_send_choice(m);
        }
    }
}

// Opt-in, read-only samples to establish which transform coordinates move
// during ordinary walking. Never log every frame or write to the game here.
void trace_cat_positions(void* comps) {
    static uint64_t last = 0;
    if (!config().house_positions || !sharing()) return;
    const uint64_t now = GetTickCount64();
    if (now - last < 500) return;
    last = now;
    List cats, places;
    if (!list_of(comps, kComps_Cats, cats) || !list_of(comps, kComps_Places, places)) return;
    for (uint32_t i = 0; i < cats.n; ++i) {
        void* cat = item(cats, i);
        void* transform = nullptr;
        void* holder = nullptr;
        uint64_t key = 0;
        double x = 0, y = 0;
        if (!cat || !mem_read((const uint8_t*)cat + kHouseCat_Key, &key, sizeof(key)) || !key ||
            !mem_read((const uint8_t*)cat + kHouseCat_Place, &holder, sizeof(holder)) ||
            !mem_read((const uint8_t*)cat + 0x40, &transform, sizeof(transform)) || !transform ||
            !mem_read((const uint8_t*)transform + 0x80, &x, sizeof(x)) ||
            !mem_read((const uint8_t*)transform + 0x88, &y, sizeof(y)))
            continue;
        uint32_t place = 0xFFFFFFFFu;
        for (uint32_t j = 0; holder && j < places.n; ++j)
            if (item(places, j) == holder) { place = j; break; }
        uintptr_t vt = 0;
        if (holder) mem_read(holder, &vt, sizeof(vt));
        // HouseCat::update dispatches on +0x94 and changes the adjacent
        // fields during those branches. Their precise meanings are not yet
        // verified, so record them without writing or assigning labels.
        uint32_t field90 = 0, state94 = 0;
        double fieldb8 = 0, fieldc0 = 0;
        double fieldc8 = 0;
        int32_t targetx = 0, targety = 0;
        void* physics = nullptr;
        void* anim = nullptr;
        double velocityx = 0, velocityy = 0;
        uint8_t field89 = 0, fieldd8 = 0, fieldd9 = 0;
        uint8_t anim210 = 0, anim2a0 = 0;
        const bool fields =
            mem_read((const uint8_t*)cat + 0x90, &field90, sizeof(field90)) &&
            mem_read((const uint8_t*)cat + 0x94, &state94, sizeof(state94)) &&
            mem_read((const uint8_t*)cat + 0xB8, &fieldb8, sizeof(fieldb8)) &&
            mem_read((const uint8_t*)cat + 0xC0, &fieldc0, sizeof(fieldc0)) &&
            mem_read((const uint8_t*)cat + 0xC8, &fieldc8, sizeof(fieldc8)) &&
            mem_read((const uint8_t*)cat + 0x89, &field89, sizeof(field89)) &&
            mem_read((const uint8_t*)cat + 0xD8, &fieldd8, sizeof(fieldd8)) &&
            mem_read((const uint8_t*)cat + 0xD9, &fieldd9, sizeof(fieldd9)) &&
            mem_read((const uint8_t*)cat + 0xF0, &targetx, sizeof(targetx)) &&
            mem_read((const uint8_t*)cat + 0xF4, &targety, sizeof(targety));
        const bool velocity =
            mem_read((const uint8_t*)cat + 0x38, &physics, sizeof(physics)) && physics &&
            mem_read((const uint8_t*)physics + 0x48, &velocityx, sizeof(velocityx)) &&
            mem_read((const uint8_t*)physics + 0x50, &velocityy, sizeof(velocityy));
        const bool anim_read =
            mem_read((const uint8_t*)cat + 0x68, &anim, sizeof(anim)) && anim &&
            mem_read((const uint8_t*)anim + 0x210, &anim210, sizeof(anim210)) &&
            mem_read((const uint8_t*)anim + 0x2A0, &anim2a0, sizeof(anim2a0));
        // The game's animation setter writes these std::strings. Their exact
        // semantics are still under investigation; neither is modified here.
        char anim150[64] = {}, anim170[64] = {};
        const bool anim_names = anim &&
            mem_read_std_string((const uint8_t*)anim + 0x150, anim150, sizeof(anim150)) &&
            mem_read_std_string((const uint8_t*)anim + 0x170, anim170, sizeof(anim170));
        char action220[64] = {}, action240[64] = {};
        const bool action_names = anim_read && anim2a0 &&
            mem_read_std_string((const uint8_t*)anim + 0x220, action220, sizeof(action220)) &&
            mem_read_std_string((const uint8_t*)anim + 0x240, action240, sizeof(action240));
        char action190[64] = {}, action1b0[64] = {};
        const bool active_names = anim_read && anim210 &&
            mem_read_std_string((const uint8_t*)anim + 0x190, action190, sizeof(action190)) &&
            mem_read_std_string((const uint8_t*)anim + 0x1B0, action1b0, sizeof(action1b0));
        log_line("CATPOS", "cat %016llx holder=%s place=%u vtable_rva=%llx x=%.2f y=%.2f fields=%u state94=%u f90=%u f89=%u b8=%.3f c0=%.3f d8=%u d9=%u target=%d,%d velocity_read=%u vx=%.3f vy=%.3f c8=%.3f anim_read=%u anim210=%u anim2a0=%u anim_names=%u a150='%s' a170='%s' action_names=%u a220='%s' a240='%s' active_names=%u a190='%s' a1b0='%s'",
                 (unsigned long long)key, holder ? kind_name(kind_of(holder)) : "none", place,
                 (unsigned long long)(vt >= g_base ? vt - g_base : 0), x, y,
                 fields ? 1u : 0u, state94, field90, (unsigned)field89, fieldb8, fieldc0,
                 (unsigned)fieldd8, (unsigned)fieldd9, targetx, targety, velocity ? 1u : 0u,
                 velocityx, velocityy, fieldc8, anim_read ? 1u : 0u,
                 (unsigned)anim210, (unsigned)anim2a0, anim_names ? 1u : 0u, anim150, anim170,
                 action_names ? 1u : 0u, action220, action240,
                 active_names ? 1u : 0u, action190, action1b0);
    }
}

// THE PIPE. A cat let go over the pipe goes to the NPC at once and is out of
// the House by the drop's end, its +0xE8 null like a cat in the yard -- and
// the other game was told "outside" and put it in the yard (2026-11-15). The
// pipe's own take (HousePipe slot 31, 0x208C40, from the place add) says which
// cat went in; the other game puts the same cat in its pipe the same way, and
// its pipe hands it over the same way.
constexpr uintptr_t kRva_PipeTake = 0x208C40;
using fn_take = void* (__fastcall*)(void*, void*, void*, void*);
fn_take o_pipe_take = nullptr;
void* g_pipe_cat  = nullptr;   // the cat the pipe took during this drop
void* g_pipe_into = nullptr;
// Not from the drop, it turned out (2026-11-15): the cat let go lands by the
// pipe (the drop finds no place: "outside"), and the pipe's own update
// (0x2082D0) takes any cat standing in its bounds with the place add a frame or
// so later. So the take itself is what goes over, as a move into the pipe.
bool sharing();
bool pipe_index(void* pipe, uint32_t& idx, uint32_t& count);
void* __fastcall pipe_take(void* pipe, void* cat, void* c, void* d) {
    if (!g_applying && remote_drag_cat(cat)) {
        drag_guard_note(cat, false);
        return nullptr;
    }
    g_pipe_cat  = cat;
    g_pipe_into = pipe;
    void* r = o_pipe_take(pipe, cat, c, d);
    uint64_t key = 0;
    uint32_t idx = 0, count = 0;
    if (((uintptr_t)r & 0xFF) && !g_applying && sharing() && cat &&
        mem_read((const uint8_t*)cat + kHouseCat_Key, &key, sizeof(key)) && key && pipe_index(pipe, idx, count)) {
        ChoiceMsg m{};
        m.kind  = kChoiceHouseMove;
        m.cat   = key;
        m.index = idx;
        m.count = count;
        m.aux   = 2;
        net_send_choice(m);
        log_line("HOUSE", "-> %s (%016llx) taken in by the pipe (place %u of %u)", night_cat_name(key).c_str(),
                 (unsigned long long)key, idx, count);
    }
    return r;
}

// THE NPC PICKED. Taken in by the pipe, the cat opens the NPC map
// (sub_140279920 from the pipe's update, 0x20493F: drawer+0x40 set, the cat at
// +0x48, "{Catname} is sent to:"). Each NPC on it is a lambda, closure+8 the
// drawer: sub_1401C1650(progression, npc, catdata, 1) gives the cat over,
// drawer+0xB8 = npc, the cat taken out of its place and its entity destroyed,
// +0x48 cleared. npc 0..7 by _Do_call: 0x27EC20 0, 0x27EB20 1, 0x27ED20 2,
// 0x27E720 3, 0x27EA20 4, 0x27E920 5, 0x27E820 6, 0x27E640 7.
// Both maps opened, each player picked for themself (2026-11-15). Now the one
// who sent the cat picks: the other map does not take a click for that cat,
// and the pick comes over and runs the same lambda there.
constexpr uintptr_t kDrawer_PipeOn  = 0x40;
constexpr uintptr_t kDrawer_PipeCat = 0x48;
constexpr uintptr_t kDrawer_Choice  = 0xB8;
const uintptr_t kPickRva[8] = { 0x27EC20, 0x27EB20, 0x27ED20, 0x27E720, 0x27EA20, 0x27E920, 0x27E820, 0x27E640 };
const uint8_t kPickHead[10] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20 };
using fn_pick = void (__fastcall*)(void*);
fn_pick  o_pick[8] = {};
bool     g_delivering = false;
uint64_t g_pipe_remote_key = 0, g_pipe_remote_at = 0;   // the other player's cat in this game's pipe
bool     o_deliver_ok = false;                          // the picks hooked

void*    g_watch_drawer = nullptr;
uint64_t g_watch_until  = 0;
uint8_t  g_watch_prev[0xC8];
bool     g_watch_prev_ok = false;

void pick(uint32_t npc, void* closure) {
    void* drawer = nullptr;
    void* cat = nullptr;
    uint64_t key = 0;
    if (closure && mem_read((const uint8_t*)closure + 8, &drawer, sizeof(drawer)) && drawer &&
        mem_read((const uint8_t*)drawer + kDrawer_PipeCat, &cat, sizeof(cat)) && cat)
        mem_read((const uint8_t*)cat + kHouseCat_Key, &key, sizeof(key));
    const bool linked = net_active() && net_peer_count() >= 2;
    if (!g_delivering && linked && key && key == g_pipe_remote_key &&
        GetTickCount64() - g_pipe_remote_at < 120000) {
        log_line("HOUSE", "the other player sent %s down the pipe -- they pick the NPC, not this click",
                 night_cat_name(key).c_str());
        return;
    }
    o_pick[npc](closure);
    if (!g_delivering && drawer) { g_watch_drawer = drawer; g_watch_until = GetTickCount64() + 3000; g_watch_prev_ok = false; }
    if (!g_delivering && linked && key) {
        ChoiceMsg m{};
        m.kind  = kChoicePipeSend;
        m.cat   = key;
        m.index = npc;
        net_send_choice(m);
        log_line("HOUSE", "-> %s (%016llx) sent down the pipe to NPC %u", night_cat_name(key).c_str(),
                 (unsigned long long)key, npc);
    }
}
#define MGMP_PICK(i) void __fastcall pick##i(void* c) { pick(i, c); }
MGMP_PICK(0) MGMP_PICK(1) MGMP_PICK(2) MGMP_PICK(3) MGMP_PICK(4) MGMP_PICK(5) MGMP_PICK(6) MGMP_PICK(7)
#undef MGMP_PICK
void* const kPickDetour[8] = { (void*)&pick0, (void*)&pick1, (void*)&pick2, (void*)&pick3,
                               (void*)&pick4, (void*)&pick5, (void*)&pick6, (void*)&pick7 };

// Diagnostic (2026-11-15): the map's fields as it closes after a pick here.
void watch_drawer(void* drawer) {
    if (drawer != g_watch_drawer || GetTickCount64() > g_watch_until) return;
    uint8_t now[0xC8] = {};
    mem_read((const uint8_t*)drawer + 0x38, now, sizeof(now));
    if (g_watch_prev_ok) {
        std::string diff;
        char b[48];
        for (size_t i = 0; i < sizeof(now); ++i)
            if (now[i] != g_watch_prev[i]) {
                snprintf(b, sizeof(b), " +0x%zx:%02x>%02x", i + 0x38, g_watch_prev[i], now[i]);
                diff += b;
            }
        if (!diff.empty()) log_line("HOUSE", "map after the pick:%s", diff.c_str());
    } else {
        std::string hx; char b[4];
        for (size_t i = 0; i < sizeof(now); ++i) { snprintf(b, sizeof(b), "%02x", now[i]); hx += b; if ((i & 7) == 7) hx += ' '; }
        log_line("HOUSE", "map right after the pick, +0x38: %s", hx.c_str());
    }
    memcpy(g_watch_prev, now, sizeof(now));
    g_watch_prev_ok = true;
}

// POOPS BY DAY. A cat poops by day through FurnitureGrid's slot 34
// (0x2EA9A0: a "poop" record, its piece by the cat), each game with its own
// cats' chance: one game had a poop the other had not, and the morning came
// out different (2026-11-15). By day the client makes none; the host's come
// over as furniture (furniture_watch). The night's are made on both alike.
constexpr uintptr_t kRva_GridVtbl  = 0xEFCDE8;
constexpr uintptr_t kRva_PoopSpawn = 0x2EA9A0;
using fn_spawn = void* (__fastcall*)(void*, void*, void*, void*);
fn_spawn o_poop_spawn = nullptr;
extern uint64_t g_night_at_pub();
void* __fastcall poop_spawn(void* a, void* b, void* c, void* d) {
    if (net_role() != NetRole::Host && net_active() && net_peer_count() >= 2 && leave_on_house() &&
        !g_night_at_pub())
        return nullptr;   // the host's poop comes over instead
    return o_poop_spawn(a, b, c, d);
}

bool safe_pick(uint32_t npc, void* drawer) {
    void* closure[2] = { nullptr, drawer };
    __try { o_pick[npc](closure); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Butch's box (component 0x1AC, comps+0x1AC0, as sub_1400ECD50 reads it)
// keeps the cats picked for a run in its four slots at +0x100, and a cat let
// go into it keeps +0xE8 null like one in the yard: the friend's drop into the
// box went over as "outside" as well, the host's game took Korova out of its
// box for it and put her back as its own pick (2026-11-15). The box has its
// own sync (mgmp_startrun); its cats are left to it.
extern void* g_comps;
bool in_box(void* cat) {
    if (!g_comps || !cat) return false;
    List boxes;
    if (!list_of(g_comps, 0x1AC0, boxes)) return false;
    for (uint32_t b = 0; b < boxes.n; ++b) {
        void* box = item(boxes, b);
        void* slots = nullptr;
        void* in[4] = {};
        if (!box || !mem_read((const uint8_t*)box + 0x100, &slots, sizeof(slots)) || !slots ||
            !mem_read(slots, in, sizeof(in)))
            continue;
        for (void* c : in) if (c == cat) return true;
    }
    return false;
}

bool pipe_index(void* pipe, uint32_t& idx, uint32_t& count) {
    void* comps = nullptr;
    List places;
    if (!comps_of(pipe, comps) || !list_of(comps, kComps_Places, places)) return false;
    for (uint32_t i = 0; i < places.n; ++i)
        if (item(places, i) == pipe) { idx = i; count = places.n; return true; }
    return false;
}

void __fastcall drop(void* drag) {
    void* cat = nullptr;
    void* was = nullptr;
    uint64_t key = 0;
    if (mem_read((const uint8_t*)drag + kDrag_Cat, &cat, sizeof(cat)) && cat) {
        mem_read((const uint8_t*)cat + kHouseCat_Key, &key, sizeof(key));
        mem_read((const uint8_t*)cat + kHouseCat_Place, &was, sizeof(was));
    }
    g_pipe_cat = nullptr;
    if (drag_ready() && key && !drag_move_allowed(key, net_self())) {
        void* none = nullptr;
        mem_write((uint8_t*)drag + kDrag_Cat, &none, sizeof(none));
        return; // losing a simultaneous pickup must not place the winner's cat
    }
    drag_publish_final(cat);
    o_drop(drag);
    if (!cat || !key || !sharing()) return;
    // Do not immediately overwrite a local player's newly placed cat with an
    // older host sample while the host is processing the placement.
    g_walk_pause_until = GetTickCount64() + 3000;
    void* now = nullptr;
    mem_read((const uint8_t*)cat + kHouseCat_Place, &now, sizeof(now));
    g_pipe_cat = nullptr;
    if (!now && in_box(cat)) return;   // into Butch's box: its own sync
    if (!now) {
        ChoiceMsg m{};
        m.kind  = kChoiceHouseMove;
        m.cat   = key;
        m.index = kOutside;
        if (drag_ready()) m.node_seed = g_drag_sequence;
        net_send_choice(m);
        log_line("HOUSE", "-> %s (%016llx) put outside", night_cat_name(key).c_str(), (unsigned long long)key);
        return;
    }
    const uint32_t kind = kind_of(now);
    if (kind == 4) return;   // Butch's box has its own
    void* comps = nullptr;
    List places;
    if (!comps_of(now, comps) || !list_of(comps, kComps_Places, places)) {
        log_line("HOUSE", "!! cat %016llx's new place is not on the House's list -- not sent", (unsigned long long)key);
        return;
    }
    for (uint32_t i = 0; i < places.n; ++i) {
        if (item(places, i) != now) continue;
        ChoiceMsg m{};
        m.kind  = kChoiceHouseMove;
        m.cat   = key;
        m.index = i;
        m.count = places.n;
        m.aux   = kind;
        if (drag_ready()) m.node_seed = g_drag_sequence;
        net_send_choice(m);
        const std::string name = night_cat_name(key);
        log_line("HOUSE", "-> %s (%016llx) put in %s %u of %u%s", name.c_str(), (unsigned long long)key,
                 kind_name(kind), i, places.n, was == now ? " (where it was)" : "");
        return;
    }
    log_line("HOUSE", "!! cat %016llx's new place is not on the House's list -- not sent", (unsigned long long)key);
}

bool safe_place(void* place, void* cat) {
    __try { place_add(place, cat); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool safe_unplace(void* place, void* cat) {
    __try { place_remove(place, cat); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Visible drag prototype. We never put the remote cat in the local hand.
// Suspend only its HouseCat/HouseCatPhysics updates, leaving the renderer alive.
using fn_drag_update = void (__fastcall*)(void*);
fn_drag_update o_drag_input = nullptr, o_drag_cat = nullptr, o_drag_physics = nullptr;
bool g_drag_hooks = false;
void* g_drag_scene = nullptr;
struct DragVisual {
    void* cat = nullptr; uint64_t cookie = 0;
    uint8_t peer = 255;
    uint32_t home = kOutside, places = 0;
    int32_t depth = 0;
    double home_x = 0, home_y = 0, x = 0, y = 0;
    uint64_t deadline = 0, released_at = 0, frame_at = 0, trace_at = 0;
    bool pipe_blocked = false, box_blocked = false;
};
std::map<uint64_t, DragVisual> g_drag_visuals;

bool drag_ready() {
    return config().house_drag_sync && g_drag_hooks && place_remove &&
           o_pipe_take && startrun_box_drag_guard_ready();
}

uint64_t cat_key(void* cat) {
    uint64_t key = 0;
    if (cat) mem_read((const uint8_t*)cat + kHouseCat_Key, &key, sizeof(key));
    return key;
}

bool drag_identity(void* cat, const DragVisual& v) {
    uint64_t cookie = 0;
    void* comps = nullptr;
    return cat && cat == v.cat &&
           mem_read((const uint8_t*)cat - 8, &cookie, sizeof(cookie)) && cookie == v.cookie &&
           comps_of(cat, comps) && comps == g_drag_scene;
}

void* drag_hand(void* comps, void*& cat) {
    List list;
    cat = nullptr;
    void* drag = list_of(comps, 0x4490, list) ? item(list, 0) : nullptr;
    if (drag) mem_read((const uint8_t*)drag + kDrag_Cat, &cat, sizeof(cat));
    return drag;
}

void drag_position(void* cat, double x, double y) {
    void* transform = nullptr;
    if (mem_read((const uint8_t*)cat + 0x40, &transform, sizeof(transform)) && transform) {
        const double pos[2] = { x, y };
        mem_write((uint8_t*)transform + 0x80, pos, sizeof(pos));
    }
}

void drag_depth(void* cat, int32_t depth) {
    mem_write((uint8_t*)cat + 0xF8, &depth, sizeof(depth));
    void* transform = nullptr;
    if (mem_read((const uint8_t*)cat + 0x40, &transform, sizeof(transform)) && transform) {
        const double z = -(double)depth;
        mem_write((uint8_t*)transform + 0x68, &z, sizeof(z));
        mem_write((uint8_t*)transform + 0xB0, &z, sizeof(z));
    }
}

// Do not leave the held-to-released displacement in the physics history:
// otherwise the next physics tick interprets the network jump as a throw.
void drag_stop_physics(void* cat, double x, double y) {
    void* physics = nullptr;
    if (!mem_read((const uint8_t*)cat + 0x38, &physics, sizeof(physics)) || !physics) return;
    const double zero[2] = {};
    const double pos[2] = { x, y };
    const uint8_t not_held = 0;
    mem_write((uint8_t*)physics + 0x48, zero, sizeof(zero));
    mem_write((uint8_t*)physics + 0x58, pos, sizeof(pos));
    mem_write((uint8_t*)physics + 0x68, &not_held, 1);
}

bool drag_safe_room(void* place) {
    uintptr_t vt = 0;
    return place && mem_read(place, &vt, sizeof(vt)) &&
           (vt == g_base + kRva_RoomVtbl || vt == g_base + kRva_GridVtbl);
}

bool remote_drag_cat(void* cat) {
    if (!drag_ready() || !sharing()) return false;
    auto it = g_drag_visuals.find(cat_key(cat));
    return it != g_drag_visuals.end() && drag_identity(cat, it->second) &&
           GetTickCount64() <= it->second.deadline;
}

void drag_guard_note(void* cat, bool box) {
    auto it = g_drag_visuals.find(cat_key(cat));
    if (it == g_drag_visuals.end()) return;
    bool& said = box ? it->second.box_blocked : it->second.pipe_blocked;
    if (said) return;
    said = true;
    log_line("HOUSEDRAG", "blocked automatic %s pickup of held cat %016llx",
             box ? "box" : "pipe", (unsigned long long)it->first);
}

bool drag_held_pose(void* cat) {
    // HouseCat::update calls this exact command with enum 8 and an owned
    // _DEFAULT string while held. The game consumes the string argument.
    struct InlineString { char text[16]; uint64_t size, capacity; };
    InlineString name{{'_','D','E','F','A','U','L','T',0}, 8, 15};
    void* animation = nullptr;
    if (!mem_read((const uint8_t*)cat + 0x68, &animation, sizeof(animation)) || !animation) return false;
    const uint8_t head[] = {0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x74,0x24,0x20};
    if (memcmp((const void*)(g_base + 0x9AA360), head, sizeof(head))) return false;
    using SetPose = void (__fastcall*)(void*, int, void*);
    __try {
        ((SetPose)(g_base + 0x9AA360))(animation, 8, &name);
        const uint8_t held = 1;
        mem_write((uint8_t*)animation + 0xB0, &held, 1);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool drag_move_allowed(uint64_t key, uint8_t from) {
    if (!drag_ready() || from == kNoPeer) return true;
    const uint8_t owner = g_drag_tracker.owner(key);
    if (owner != kNoPeer && owner < from) return false;
    if (g_local_drag_key == key && net_self() < from) return false;
    auto it = g_drag_visuals.find(key);
    return it == g_drag_visuals.end() || it->second.peer >= from;
}

void finish_remote_drag(void* cat, bool restore) {
    auto it = g_drag_visuals.find(cat_key(cat));
    if (it == g_drag_visuals.end() || !drag_identity(cat, it->second)) return;
    const DragVisual v = it->second;
    const uint64_t key = it->first;
    g_drag_visuals.erase(it); // stop guards before invoking a real placement
    double x = v.x, y = v.y;
    void* holder = nullptr;
    mem_read((const uint8_t*)cat + kHouseCat_Place, &holder, sizeof(holder));
    if (restore && !holder && !in_box(cat)) {
        List places;
        if (v.home != kOutside && list_of(g_drag_scene, kComps_Places, places) &&
            places.n == v.places) {
            void* home = item(places, v.home);
            if (drag_safe_room(home)) safe_place(home, cat);
        }
        x = v.home_x; y = v.home_y;
        drag_position(cat, x, y);
    }
    drag_depth(cat, v.depth + (g_local_drag_key == key ? 100000 : 0));
    drag_stop_physics(cat, x, y);
    log_line("HOUSEDRAG", "%s remote cat %016llx peer=%u",
             restore ? "restored" : "placed", (unsigned long long)key, (unsigned)v.peer);
}

void drag_publish_final(void* cat) {
    if (!drag_ready() || !sharing() || !cat) return;
    void* transform = nullptr;
    double pos[2] = {};
    if (!mem_read((const uint8_t*)cat + 0x40, &transform, sizeof(transform)) || !transform ||
        !mem_read((const uint8_t*)transform + 0x80, pos, sizeof(pos)) ||
        !std::isfinite(pos[0]) || !std::isfinite(pos[1]) ||
        std::abs(pos[0]) > 10000 || std::abs(pos[1]) > 10000) return;
    const float x = (float)pos[0], y = (float)pos[1];
    ChoiceMsg m{};
    m.kind = kChoiceHouseDragTrace; m.cat = cat_key(cat); m.index = 1;
    m.node_seed = ++g_drag_sequence;
    memcpy(&m.count, &x, sizeof(x)); memcpy(&m.aux, &y, sizeof(y));
    net_send_choice(m);
}

void __fastcall drag_cat_update(void* cat) {
    if (remote_drag_cat(cat)) {
        const auto& v = g_drag_visuals.find(cat_key(cat))->second;
        drag_position(cat, v.x, v.y);
        return;
    }
    o_drag_cat(cat);
}

void __fastcall drag_physics_update(void* physics) {
    void* cat = nullptr;
    mem_read((const uint8_t*)physics + 0x40, &cat, sizeof(cat));
    if (cat && remote_drag_cat(cat)) {
        const auto& v = g_drag_visuals.find(cat_key(cat))->second;
        drag_position(cat, v.x, v.y);
        drag_stop_physics(cat, v.x, v.y);
        return;
    }
    o_drag_physics(physics);
}

void drag_cancel_losing_hand(void* drag) {
    void* cat = nullptr;
    if (!drag_ready() || !sharing() ||
        !mem_read((const uint8_t*)drag + kDrag_Cat, &cat, sizeof(cat)) || !cat) return;
    const uint64_t key = cat_key(cat);
    if (drag_move_allowed(key, net_self())) return;
    auto visual = g_drag_visuals.find(key);
    if (visual != g_drag_visuals.end()) {
        drag_depth(cat, visual->second.depth + 100000);
    } else {
        int32_t depth = 0;
        if (mem_read((const uint8_t*)cat + 0xF8, &depth, sizeof(depth)) && depth >= 100000)
            drag_depth(cat, depth - 100000); // undo this losing local pickup
    }
    void* none = nullptr;
    mem_write((uint8_t*)drag + kDrag_Cat, &none, sizeof(none));
    g_local_drag_key = 0;
    log_line("HOUSEDRAG", "cancelled local pickup of %016llx: lower peer id owns it",
             (unsigned long long)key);
}

void __fastcall drag_input_update(void* drag) {
    drag_cancel_losing_hand(drag);
    o_drag_input(drag);
    drag_cancel_losing_hand(drag);
}

void drag_tick(void* comps) {
    if (comps != g_drag_scene) {
        // Never dereference pointers from an old scene.
        g_drag_visuals.clear();
        g_drag_tracker.reset();
        g_drag_scene = comps;
        g_drag_previous = g_local_drag_key = 0;
    }
    void* local = nullptr;
    void* hand = drag_hand(comps, local);
    g_local_drag_key = cat_key(local);
    const uint64_t now = GetTickCount64();
    std::vector<DragMessage> messages;
    if (g_cs_ready) {
        EnterCriticalSection(&g_cs);
        messages.swap(g_drag_messages);
        LeaveCriticalSection(&g_cs);
    }
    if (!sharing()) {
        for (auto it = g_drag_visuals.begin(); it != g_drag_visuals.end();) {
            const DragVisual v = (it++)->second;
            if (drag_identity(v.cat, v)) finish_remote_drag(v.cat, true);
        }
        g_drag_visuals.clear();
        g_drag_tracker.reset();
        return;
    }
    for (const auto& incoming : messages) {
        const ChoiceMsg& m = incoming.msg;
        const uint8_t from = incoming.from;
        bool accepted = false;
        if (!m.index) {
            accepted = g_drag_tracker.release(from, m.cat, m.node_seed);
            auto it = g_drag_visuals.find(m.cat);
            if (accepted && it != g_drag_visuals.end() && it->second.peer == from) {
                it->second.released_at = now;
                it->second.deadline = now + 700; // wait for room/pipe/box placement
            }
        } else {
            float x = 0, y = 0;
            memcpy(&x, &m.count, sizeof(x)); memcpy(&y, &m.aux, sizeof(y));
            accepted = g_drag_tracker.sample(from, m.cat, m.node_seed, x, y,
                                               now, g_local_drag_key, net_self());
        }
        if (!m.index || !accepted || !drag_ready())
            log_line("HOUSEDRAG", "<- peer %u %s cat %016llx seq=%llu %s%s",
                     (unsigned)from, m.index ? "holds" : "released",
                     (unsigned long long)m.cat, (unsigned long long)m.node_seed,
                     accepted ? "accepted" : "ignored", drag_ready() ? "" : " (diagnostic only)");
    }
    g_drag_tracker.expire(now);
    g_drag_tracker.exclude_local(g_local_drag_key, net_self());
    if (hand) drag_cancel_losing_hand(hand);
    if (!drag_ready()) return;
    for (auto it = g_drag_visuals.begin(); it != g_drag_visuals.end();) {
        const auto key = it->first;
        const DragVisual v = (it++)->second;
        if (!drag_identity(v.cat, v)) { g_drag_visuals.erase(key); continue; }
        const uint8_t owner = g_drag_tracker.owner(key);
        if (now > v.deadline || (owner == kNoPeer && !v.released_at) ||
            (owner != kNoPeer && owner != v.peer))
            finish_remote_drag(v.cat, g_local_drag_key != key);
    }
    List cats, places;
    if (!list_of(comps, kComps_Cats, cats) || !list_of(comps, kComps_Places, places)) return;
    for (unsigned peer = 0; peer < kNoPeer; ++peer) {
        const auto& s = g_drag_tracker.slot((uint8_t)peer);
        if (!s.key) continue;
        auto found = g_drag_visuals.find(s.key);
        if (found == g_drag_visuals.end()) {
            void* cat = nullptr;
            for (uint32_t i = 0; i < cats.n && !cat; ++i) {
                void* c = item(cats, i);
                if (cat_key(c) == s.key) cat = c;
            }
            if (!cat || in_box(cat)) continue; // box's own removal must finish first
            void* holder = nullptr; void* transform = nullptr;
            DragVisual v;
            v.cat = cat; v.peer = (uint8_t)peer; v.places = places.n;
            if (!mem_read((const uint8_t*)cat - 8, &v.cookie, sizeof(v.cookie)) ||
                !mem_read((const uint8_t*)cat + 0xF8, &v.depth, sizeof(v.depth)) ||
                v.depth > INT32_MAX - 100000 || v.depth < -1000000 ||
                !mem_read((const uint8_t*)cat + kHouseCat_Place, &holder, sizeof(holder)) ||
                !mem_read((const uint8_t*)cat + 0x40, &transform, sizeof(transform)) || !transform ||
                !mem_read((const uint8_t*)transform + 0x80, &v.home_x, sizeof(v.home_x)) ||
                !mem_read((const uint8_t*)transform + 0x88, &v.home_y, sizeof(v.home_y))) continue;
            if (holder) {
                if (!drag_safe_room(holder)) continue; // never take a cat back out of a pipe
                for (uint32_t i = 0; i < places.n; ++i)
                    if (item(places, i) == holder) v.home = i;
                if (v.home == kOutside || !safe_unplace(holder, cat)) continue;
                void* none = nullptr;
                mem_write((uint8_t*)cat + kHouseCat_Place, &none, sizeof(none));
            }
            v.x = s.x; v.y = s.y; v.frame_at = now;
            found = g_drag_visuals.emplace(s.key, v).first;
            drag_depth(cat, v.depth + 100000);
            if (!drag_held_pose(cat))
                log_line("HOUSEDRAG", "!! held pose unavailable for cat %016llx",
                         (unsigned long long)s.key);
            log_line("HOUSEDRAG", "visible remote pickup cat %016llx peer=%u",
                     (unsigned long long)s.key, peer);
        }
        auto& v = found->second;
        const double alpha = std::fmin(1.0, (double)(now - v.frame_at) / 60.0);
        v.x += (s.x - v.x) * alpha; v.y += (s.y - v.y) * alpha;
        v.frame_at = now; v.deadline = s.last_at + HouseDragTracker::kLeaseMs;
        v.released_at = 0;
        drag_position(v.cat, v.x, v.y);
        drag_stop_physics(v.cat, v.x, v.y);
        if (config().house_positions && now - v.trace_at >= 200) {
            v.trace_at = now;
            log_line("HOUSEDRAG", "showing cat %016llx peer=%u x=%.3f y=%.3f target=%.3f,%.3f",
                     (unsigned long long)s.key, peer, v.x, v.y, (double)s.x, (double)s.y);
        }
    }
}

void drag_install() {
    if (!config().house_drag_sync) return;
    struct Hook { uintptr_t rva; const char* head; void* det; fn_drag_update* orig; };
    const Hook hooks[] = {
        {0x1FACC0, "\x48\x8b\xc4\x55\x53\x56\x57\x41\x54\x41", (void*)&drag_cat_update, &o_drag_cat},
        {0x1FF3F0, "\x48\x89\x5c\x24\x08\x57\x48\x81\xec\x80", (void*)&drag_physics_update, &o_drag_physics},
        {0x1FE940, "\x48\x8b\xc4\x48\x89\x58\x18\x55\x56\x57", (void*)&drag_input_update, &o_drag_input}
    };
    for (const auto& h : hooks) {
        void* at = (void*)(g_base + h.rva);
        if (memcmp(at, h.head, 10) != 0 ||
            MH_CreateHook(at, h.det, (void**)h.orig) != MH_OK ||
            MH_EnableHook(at) != MH_OK) {
            log_line("HOUSEDRAG", "!! safety hook %llx failed; visible dragging disabled",
                     (unsigned long long)h.rva);
            return;
        }
    }
    g_drag_hooks = true;
    log_line("HOUSEDRAG", "EXPERIMENTAL visible drag hooks installed; waiting for pipe/box guards");
}

// Out of every place's list (the one it is in, and any stale one), +0xE8 null.
void put_outside(const List& places, void* cat, const std::string& name, uint64_t key) {
    void* was = nullptr;
    mem_read((const uint8_t*)cat + kHouseCat_Place, &was, sizeof(was));
    bool ok = true;
    uint32_t out = 0;
    for (uint32_t i = 0; i < places.n; ++i) {
        void* p = item(places, i);
        uint32_t n = 0; void** data = nullptr;
        if (p && kind_of(p) == 4) continue;   // never out of Butch's box: its own sync
        if (!p || !mem_read((const uint8_t*)p + 0x6C, &n, sizeof(n)) ||
            !mem_read((const uint8_t*)p + 0x70, &data, sizeof(data)) || !data || n > 4096)
            continue;
        for (uint32_t j = 0; j < n; ++j) {
            void* c = nullptr;
            if (mem_read(&data[j], &c, sizeof(c)) && c == cat) {
                ok &= safe_unplace(p, cat);
                ++out;
                break;
            }
        }
    }
    if (!was && !out) return;   // outside already
    void* none = nullptr;
    ok &= mem_write((uint8_t*)cat + kHouseCat_Place, &none, sizeof(none));
    log_line("HOUSE", ok ? "<- %s (%016llx) put outside, as the other player did"
                         : "!! putting %s (%016llx) outside faulted", name.c_str(), (unsigned long long)key);
}

void apply(void* comps, const Move& mv) {
    if (!drag_move_allowed(mv.key, mv.from)) {
        log_line("HOUSEDRAG", "ignored losing placement cat %016llx peer=%u",
                 (unsigned long long)mv.key, (unsigned)mv.from);
        return;
    }
    if (drag_ready() && mv.from != kNoPeer && mv.drag_sequence) {
        const auto& s = g_drag_tracker.slot(mv.from);
        if (s.key == mv.key && s.sequence > mv.drag_sequence) return;
    }
    List places, cats;
    const std::string name = night_cat_name(mv.key);
    if (!list_of(comps, kComps_Places, places) || !list_of(comps, kComps_Cats, cats)) {
        log_line("HOUSE", "!! this House's lists did not read -- %s stays where it is", name.c_str());
        return;
    }
    void* cat = nullptr;
    for (uint32_t i = 0; i < cats.n && !cat; ++i) {
        void* c = item(cats, i);
        uint64_t k = 0;
        if (c && mem_read((const uint8_t*)c + kHouseCat_Key, &k, sizeof(k)) && k == mv.key) cat = c;
    }
    if (!cat) {
        log_line("HOUSE", "!! cat %016llx is not in this House -- not moved", (unsigned long long)mv.key);
        return;
    }
    if (mv.index == kOutside) {
        if (!place_remove) return;
        if (in_box(cat)) {
            log_line("HOUSE", "%s is in Butch's box here -- left there (the box has its own sync)", name.c_str());
            return;
        }
        finish_remote_drag(cat, false);
        put_outside(places, cat, name, mv.key);
        return;
    }
    if (places.n != mv.count) {
        log_line("HOUSE", "!! this House has %u places and the other player's %u -- %s stays where it is",
                 places.n, mv.count, name.c_str());
        return;
    }
    void* place = item(places, mv.index);
    if (!place || kind_of(place) != mv.kind) {
        log_line("HOUSE", "!! place %u here is a %s, the other player's a %s -- %s stays where it is",
                 mv.index, place ? kind_name(kind_of(place)) : "nothing", kind_name(mv.kind), name.c_str());
        return;
    }
    finish_remote_drag(cat, false);
    void* was = nullptr;
    mem_read((const uint8_t*)cat + kHouseCat_Place, &was, sizeof(was));
    if (was == place) return;
    if (mv.kind == 2) {   // the other player's cat down this game's pipe: its NPC is theirs to pick
        g_pipe_remote_key = mv.key;
        g_pipe_remote_at  = GetTickCount64();
    }
    g_applying = true;
    const bool ok = safe_place(place, cat);
    g_applying = false;
    log_line("HOUSE", ok ? "<- %s (%016llx) put in %s %u, as the other player did"
                         : "!! putting %s (%016llx) in %s %u faulted",
             name.c_str(), (unsigned long long)mv.key, kind_name(mv.kind), mv.index);
}

// ---------------------------------------------------------------------------
// THE NIGHT WITHOUT A RELOAD (the user's plan, 2026-11-15). The end-day sign's
// lambda (0x1401F8EA0) takes its House at closure+8 and, if Butch's box does
// not set off a run, calls sub_1401EF470(House): it raises House+0xB0 and
// starts the move to the night, whose resolver -- sub_1401E8B40(House, out),
// one caller -- draws every event on the simulation stream. When the day-end
// check found the friend's House the host's, the friend's game ends its own
// day the same way and both resolvers start from the stream the host had as
// its sign was pressed: the same pairs, the same kittens, its own scenes. The
// morning saves are compared after (house_morning), and the host's is loaded
// only if they came out different.
//
// The House is the scene's component 0x446 (comps+0x4460, as sub_1400ECC30
// reads it), vftable 0xF10158.
constexpr uintptr_t kRva_EndTheDay  = 0x1EF470;
const uint8_t kEndTheDayHead[10]    = { 0x48, 0x83, 0xEC, 0x28, 0x80, 0xB9, 0xB0, 0x00, 0x00, 0x00 };
constexpr uintptr_t kRva_Resolve    = 0x1E8B40;
const uint8_t kResolveHead[10]      = { 0x48, 0x89, 0x54, 0x24, 0x10, 0x48, 0x89, 0x4C, 0x24, 0x08 };
constexpr uintptr_t kRva_HouseVtbl  = 0xF10158;
constexpr uintptr_t kComps_House    = 0x4460;
constexpr uintptr_t kHouse_DayEnds  = 0xB0;
constexpr uint64_t  kMatchFreshMs   = 10000;   // the check must be this recent for the night to be ours
constexpr uint64_t  kNightGoMs      = 10000;   // this game's House must come up this soon
constexpr uint64_t  kSeedMs         = 60000;   // a resolver this long after the sign does not take the seed
constexpr uint64_t  kMorningWaitMs  = 30000;   // this game's morning save, waited for this long

using fn_house   = void  (__fastcall*)(void*);
using fn_resolve = void* (__fastcall*)(void*, void*, void*, void*);
fn_house   end_the_day = nullptr;
fn_resolve o_resolve   = nullptr;
uint64_t   g_seed[4]   = {};
uint64_t   g_seed_at   = 0;     // armed: the next resolver starts from g_seed
uint64_t   g_night_go  = 0;     // client: end this game's day at the next House update
uint64_t   g_local_at  = 0;     // client: this game ended its own day then
uint64_t   g_night_at  = 0;     // either: this game's night began then
uint64_t g_night_at_pub() { return g_night_at || g_night_go || g_local_at; }
volatile uint64_t g_inter_at = 0;   // the last update of a night event screen

// glaiel::InterstitialScene, vftable 0xEFD2E8, slot 10 its update: one screen
// per event of the night, one after another.
constexpr uintptr_t kRva_InterVtbl   = 0xEFD2E8;
constexpr uintptr_t kRva_InterUpdate = 0x20D1C0;
using fn_upd = void* (__fastcall*)(void*, void*, void*, void*);
fn_upd o_inter_update = nullptr;
// THE HOST TURNS THE NIGHT'S SCREENS (the user's ask, 2026-11-15). A screen's
// OK is InterstitialScene's button lambda, _Do_call 0x20EF70 (closure: the
// screen at +8, an int at +0x10): with the outcome not out yet (+0x181 down) it
// shows it (0x20D400), else it closes the screen (0x20E3F0). The host's OKs go
// over with the screen's number in the night; the client's own do nothing in a
// shared night, and the host's are pressed on the client's screen of the same
// number, from that screen's update.
constexpr uintptr_t kRva_InterOk = 0x20EF70;
const uint8_t kInterOkHead[10]   = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x8B, 0xD9, 0x48 };
constexpr uintptr_t kInter_OutcomeOut = 0x181;
using fn_ok = void (__fastcall*)(void*);
fn_ok    o_inter_ok   = nullptr;
bool     g_pressing   = false;
void*    g_cur_scene  = nullptr;   // this game's night screen now, and its number
uint32_t g_cur_scene_n = 0;
struct NightClick { uint32_t n, action, arg; };
NightClick g_clicks[16];
uint32_t   g_click_n = 0;
bool night_shared();

void __fastcall inter_ok(void* closure) {
    void* scene = nullptr;
    uint8_t out = 0;
    uint32_t arg = 0;
    if (closure) {
        mem_read((const uint8_t*)closure + 8, &scene, sizeof(scene));
        mem_read((const uint8_t*)closure + 0x10, &arg, sizeof(arg));
    }
    if (scene) mem_read((const uint8_t*)scene + kInter_OutcomeOut, &out, 1);
    if (!g_pressing && night_shared() && scene == g_cur_scene) {
        if (net_role() != NetRole::Host) return;   // the host turns them
        ChoiceMsg m{};
        m.kind  = kChoiceNightClick;
        m.index = g_cur_scene_n;
        m.aux   = out ? 2 : 1;
        m.count = arg;
        net_send_choice(m);
        log_line("HOUSE", "-> night screen %u: %s", g_cur_scene_n, out ? "closed" : "outcome shown");
    }
    o_inter_ok(closure);
}

bool safe_ok(void* closure) {
    __try { o_inter_ok(closure); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void press_host_clicks(void* scene) {
    if (!g_click_n || scene != g_cur_scene || !g_cs_ready) return;
    NightClick todo[16];
    uint32_t n = 0, keep = 0;
    EnterCriticalSection(&g_cs);
    for (uint32_t i = 0; i < g_click_n; ++i) {
        if (g_clicks[i].n == g_cur_scene_n) todo[n++] = g_clicks[i];
        else if (g_clicks[i].n > g_cur_scene_n) g_clicks[keep++] = g_clicks[i];   // a screen not up here yet
    }
    g_click_n = keep;
    LeaveCriticalSection(&g_cs);
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t out = 0;
        mem_read((const uint8_t*)scene + kInter_OutcomeOut, &out, 1);
        if (todo[i].action == 1 && out) continue;   // shown here already
        if (todo[i].action == 2 && !out) {           // close: past the outcome first
            const uint8_t one = 1;
            mem_write((uint8_t*)scene + kInter_OutcomeOut, &one, 1);
        }
        void* closure[3] = { nullptr, scene, (void*)(uintptr_t)todo[i].arg };
        g_pressing = true;
        const bool ok = safe_ok(closure);
        g_pressing = false;
        log_line("HOUSE", ok ? "<- night screen %u: %s, as the host pressed"
                             : "!! pressing night screen %u (%s) faulted",
                 todo[i].n, todo[i].action == 2 ? "closed" : "outcome shown");
        if (todo[i].action == 2) break;
    }
}

void* __fastcall inter_update(void* a, void* b, void* c, void* d) {
    g_inter_at = GetTickCount64();
    if (net_role() != NetRole::Host) press_host_clicks(a);
    return o_inter_update(a, b, c, d);
}

// THE NIGHT'S SCENES. Each event's screen (InterstitialScene::init, and the
// cats' poses under it, sub_14073CC70) draws on the second stream, TLS+0x198,
// right after the simulation's: the same night showed other poses, a flipped
// room and a pentagram on one screen only (2026-11-15). Both games start each
// screen from the same state: the night's seed, mixed with the screen's number.
uint32_t g_scene_n  = 0;
bool     g_scenes_on = false;

uint64_t mix64(uint64_t z) {
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

void* __fastcall resolve(void* house, void* out, void* c, void* d) {
    g_scenes_on = false;
    g_scene_n   = 0;
    if (g_seed_at && GetTickCount64() - g_seed_at < kSeedMs) {
        g_scenes_on = true;
        if (uint64_t* s = rng_global_stream()) {
            memcpy(s, g_seed, sizeof(g_seed));
            log_line("HOUSE", "the night is resolved from the host's stream (%016llx...)",
                     (unsigned long long)g_seed[0]);
        }
    }
    g_seed_at = 0;
    return o_resolve(house, out, c, d);
}

bool safe_end_the_day(void* house) {
    __try { end_the_day(house); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---------------------------------------------------------------------------
// THE POOP. A poop is a piece of furniture named "poop" (a `furniture` row of
// its own, 64 bytes, its room and tile), and it counts against the room's
// Comfort until it is cleaned up. A click on a piece (sub_1402ED690) sends a
// poop to sub_1402EF9D0(piece): the "Pop_Poop" sound, its record dropped from
// the furniture store by its id (piece+0x2E0, the save's key), the piece
// destroyed and piece+0x2D8 (its kind: name std::string at +8) cleared.
// Cleaned on one screen, it stayed on the other, and the two Houses went into
// the night with different Comfort (the user, 2026-11-15). The id goes over;
// the other game cleans the piece with that id the same way.
constexpr uintptr_t kRva_PoopClean  = 0x2EF9D0;
const uint8_t kPoopCleanHead[12]    = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x50, 0x48, 0x8B, 0xD9, 0x48, 0x8B, 0x89 };
constexpr uintptr_t kRva_PieceVtbl  = 0xEE6858;   // FurniturePiece
constexpr uintptr_t kPiece_Kind     = 0x2D8;
constexpr uintptr_t kPiece_Id       = 0x2E0;
using fn_piece = void (__fastcall*)(void*);
fn_piece o_poop_clean = nullptr;
bool     g_cleaning   = false;     // the other player's clean, being done here
uint64_t g_poops[32]  = {};
uint32_t g_poop_n     = 0;
uintptr_t g_pieces_at = 0;         // comps offset of the FurniturePiece list, found once

void __fastcall poop_clean(void* piece) {
    void* kind = nullptr;
    uint64_t id = 0;
    mem_read((const uint8_t*)piece + kPiece_Kind, &kind, sizeof(kind));
    mem_read((const uint8_t*)piece + kPiece_Id, &id, sizeof(id));
    o_poop_clean(piece);
    void* after = kind;
    mem_read((const uint8_t*)piece + kPiece_Kind, &after, sizeof(after));
    if (!kind || after || g_cleaning || !net_active() || net_peer_count() < 2 || !leave_on_house()) return;
    ChoiceMsg m{};
    m.kind = kChoicePoopClean;
    m.cat  = id;
    net_send_choice(m);
    log_line("HOUSE", "-> poop %llu cleaned up", (unsigned long long)id);
}

bool safe_poop_clean(void* piece) {
    __try { o_poop_clean(piece); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---------------------------------------------------------------------------
// FURNITURE WATCH -- a diagnostic, 2026-11-15, to see how a placement goes
// before syncing it: every change to the furniture store's records and to the
// House's pieces is logged, and the store's add (0x205B00), drop (0x205D50) and
// the piece's init from a record (0x2EC7A0, piece+0x2E0 = id) with their
// callers.
using fn_radd  = uint64_t (__fastcall*)(void*, void*, uint8_t);
using fn_rdrop = void (__fastcall*)(void*, uint64_t);
using fn_pinit = void* (__fastcall*)(void*, uint64_t, void*, void*);
fn_radd  o_rec_add  = nullptr;
fn_rdrop o_rec_drop = nullptr;
fn_pinit o_piece_init = nullptr;

uint64_t __fastcall rec_add(void* store, void* name, uint8_t flag) {
    char n[64] = {};
    mem_read_std_string(name, n, sizeof(n));
    const uintptr_t from = (uintptr_t)_ReturnAddress() - g_base;
    const uint64_t id = o_rec_add(store, name, flag);
    log_line("FURNWATCH", "record add '%s' flag %u -> id %llu (from +0x%llx)", n, flag,
             (unsigned long long)id, (unsigned long long)from);
    return id;
}

void __fastcall rec_drop(void* store, uint64_t id) {
    log_line("FURNWATCH", "record drop id %llu (from +0x%llx)", (unsigned long long)id,
             (unsigned long long)((uintptr_t)_ReturnAddress() - g_base));
    o_rec_drop(store, id);
}

void* __fastcall piece_init(void* piece, uint64_t id, void* c, void* d) {
    log_line("FURNWATCH", "piece %p init from record %llu (from +0x%llx)", piece, (unsigned long long)id,
             (unsigned long long)((uintptr_t)_ReturnAddress() - g_base));
    return o_piece_init(piece, id, c, d);
}

struct RecSnap { uint64_t id; std::string name, room; uint8_t raw[0x40]; };
std::map<uint64_t, RecSnap> g_recs;
std::map<void*, uint64_t>   g_piece_ids;
uint64_t g_watch_at = 0;

std::string hexs(const uint8_t* p, size_t n) {
    std::string s; char x[4];
    for (size_t i = 0; i < n; ++i) { snprintf(x, sizeof(x), "%02x", p[i]); s += x; if ((i & 3) == 3) s += ' '; }
    return s;
}

void furniture_watch(void* comps);
struct FurnOp { uint64_t id; uint32_t op; std::string text; int32_t x, y; };

// THE FURNITURE PANEL (glaiel::FurnitureBuildingUI, vftable 0xF10360, slot 10
// its update 0x1A5AB0) builds its storage list again only when its +0x58 is
// set (sub_1401A5E90): a piece the other player put away was gone from the
// House and not in the list until the panel was closed and opened
// (2026-11-15). Set after each of the other player's changes.
constexpr uintptr_t kRva_FurnUIVtbl   = 0xF10360;
constexpr uintptr_t kRva_FurnUIUpdate = 0x1A5AB0;
constexpr uintptr_t kFurnUI_Dirty     = 0x58;
using fn_upd2 = void* (__fastcall*)(void*, void*, void*, void*);
fn_upd2 o_furn_ui = nullptr;
volatile bool g_furn_dirty = false;
// The list alone was not enough (2026-11-15): the icons are laid out again by
// sub_1401A6680(ui, ui+0x7C, the page), as the panel's sort buttons do after
// they set the list going again (sub_1401A3FF0).
constexpr uintptr_t kRva_FurnUIRedraw = 0x1A6680;
const uint8_t kFurnUIRedrawHead[10]   = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x89, 0x50, 0x10 };
using fn_redraw = void (__fastcall*)(void*, uint32_t);
bool safe_redraw(void* ui) {
    __try {
        uint32_t page = 0;
        mem_read((const uint8_t*)ui + 0x7C, &page, sizeof(page));
        ((fn_redraw)(g_base + kRva_FurnUIRedraw))(ui, page);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// What the panel holds back as placed (a std::set<uint64_t> at +0x88), for
// the log while this is new.
std::string held_ids(void* ui) {
    void* head = nullptr;
    uint64_t n = 0;
    if (!mem_read((const uint8_t*)ui + 0x88, &head, sizeof(head)) || !head ||
        !mem_read((const uint8_t*)ui + 0x90, &n, sizeof(n)))
        return "?";
    std::string out = std::to_string(n) + ":";
    void* stack[64];
    uint32_t sp = 0, seen = 0;
    void* root = nullptr;
    if (mem_read((const uint8_t*)head + 8, &root, sizeof(root)) && root && root != head) stack[sp++] = root;
    while (sp && seen < 40) {
        void* nd = stack[--sp];
        uint8_t isnil = 1;
        if (!mem_read((const uint8_t*)nd + 0x19, &isnil, 1) || isnil) continue;
        uint64_t v = 0;
        mem_read((const uint8_t*)nd + 0x20, &v, sizeof(v));
        out += " " + std::to_string(v);
        ++seen;
        void* l = nullptr; void* r = nullptr;
        if (sp < 62 && mem_read(nd, &l, sizeof(l)) && l && l != head) stack[sp++] = l;
        if (sp < 62 && mem_read((const uint8_t*)nd + 0x10, &r, sizeof(r)) && r && r != head) stack[sp++] = r;
    }
    return out;
}

void* __fastcall furn_ui_update(void* ui, void* b, void* c, void* d) {
    const bool dirty = g_furn_dirty;
    g_furn_dirty = false;
    if (dirty) {
        const uint8_t one = 1;
        mem_write((uint8_t*)ui + kFurnUI_Dirty, &one, 1);
    }
    void* r = o_furn_ui(ui, b, c, d);
    if (dirty && memcmp((const void*)(g_base + kRva_FurnUIRedraw), kFurnUIRedrawHead, 10) == 0) {
        const uint8_t one = 1;
        mem_write((uint8_t*)ui + kFurnUI_Dirty, &one, 1);
        const bool ok = safe_redraw(ui);
        log_line("HOUSE", ok ? "the furniture panel laid out again (held as placed: %s)"
                             : "!! laying the furniture panel out again faulted (held as placed: %s)",
                 held_ids(ui).c_str());
    }
    return r;
}
std::vector<FurnOp> g_furn_ops;
void apply_furniture(void* comps, void* scene, const FurnOp& op);
void apply_purse(int32_t coins, int32_t food);

void* g_comps    = nullptr;   // the House scene's components, from the last House update
uint64_t g_comps_at = 0;

// Only random idle actions observed at call sites with an empty callback.
// Jumping, landing, pose transitions and interactions with other cats are
// intentionally excluded: those can carry game-owned callbacks or side effects.
bool short_house_action(const char* name, uint32_t state) {
    return (state == 2 && strcmp(name, "scratching") == 0) ||
           (state == 4 && (strcmp(name, "cleanSelf") == 0 || strcmp(name, "stretch") == 0));
}

struct ActionString {
    char buf[16];
    uint64_t size, capacity;
};
static_assert(sizeof(ActionString) == 0x20, "game string layout");
using fn_house_action = uint8_t (__fastcall*)(void*, void*, void*, void*, uint8_t, uint32_t);

bool safe_short_action(void* anim, const char* name) {
    constexpr uintptr_t kRva_Action = 0x9AA5B0;
    const uint8_t head[] = { 0x4C, 0x89, 0x4C, 0x24, 0x20, 0x4C, 0x89, 0x44, 0x24, 0x18 };
    uint8_t actual[sizeof(head)] = {};
    if (!anim || !mem_read((void*)(g_base + kRva_Action), actual, sizeof(actual)) ||
        memcmp(actual, head, sizeof(head)) != 0) return false;
    const size_t len = strlen(name);
    if (!len || len > 15) return false; // both strings stay in their inline buffers
    ActionString action{}, variant{};
    memcpy(action.buf, name, len + 1);
    action.size = len; action.capacity = 15;
    memcpy(variant.buf, "_DEFAULT", 9);
    variant.size = 8; variant.capacity = 15;
    // MSVC empty std::function: its target pointer at +0x38 is null. The game
    // consumes all three arguments just as it does at the observed call site.
    alignas(16) uint8_t callback[0x40] = {};
    __try {
        return ((fn_house_action)(g_base + kRva_Action))(anim, &action, &variant, callback, 0, 0) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// FurnitureGrid owns cats walking in rooms on the tested game build.
// This is deliberately limited to matching room indices and excludes cats
// being dragged, in the yard, in the pipe or in Butch's box.
void walk_tick(void* comps) {
    static uint64_t sent_at = 0;
    const uint64_t now = GetTickCount64();
    if (!config().house_walk_sync || !sharing() || g_night_at || g_night_go || g_local_at) return;
    List cats, places;
    if (!list_of(comps, kComps_Cats, cats) || !list_of(comps, kComps_Places, places)) return;
    if (net_role() == NetRole::Host) {
        static std::map<uint64_t, std::string> last_action;
        if (now - sent_at < 500) return;
        sent_at = now;
        for (uint32_t i = 0; i < cats.n; ++i) {
            void* cat = item(cats, i);
            void* holder = nullptr; void* transform = nullptr;
            uint64_t key = 0; uintptr_t vt = 0;
            double x = 0, y = 0;
            if (!cat || !mem_read((uint8_t*)cat + kHouseCat_Key, &key, sizeof(key)) || !key ||
                !mem_read((uint8_t*)cat + kHouseCat_Place, &holder, sizeof(holder)) || !holder ||
                !mem_read(holder, &vt, sizeof(vt)) || vt != g_base + kRva_GridVtbl ||
                !mem_read((uint8_t*)cat + 0x40, &transform, sizeof(transform)) || !transform ||
                !mem_read((uint8_t*)transform + 0x80, &x, sizeof(x)) ||
                !mem_read((uint8_t*)transform + 0x88, &y, sizeof(y)) ||
                !std::isfinite(x) || !std::isfinite(y) || std::abs(x) > 10000 || std::abs(y) > 10000)
                continue;
            uint32_t place = 0;
            while (place < places.n && item(places, place) != holder) ++place;
            if (place == places.n) continue;
            const float fx = (float)x, fy = (float)y;
            uint32_t state = 0;
            int32_t tx = 0, ty = 0;
            double timer = 0;
            void* anim = nullptr;
            uint8_t active = 0, queued = 0, idle_flag = 0;
            if (!mem_read((uint8_t*)cat + 0x94, &state, sizeof(state)) ||
                !mem_read((uint8_t*)cat + 0xF0, &tx, sizeof(tx)) ||
                !mem_read((uint8_t*)cat + 0xF4, &ty, sizeof(ty)) ||
                !mem_read((uint8_t*)cat + 0xC0, &timer, sizeof(timer)) ||
                !mem_read((uint8_t*)cat + 0x68, &anim, sizeof(anim)) || !anim ||
                !mem_read((uint8_t*)anim + 0x210, &active, sizeof(active)) ||
                !mem_read((uint8_t*)anim + 0x2A0, &queued, sizeof(queued)) ||
                !mem_read((uint8_t*)cat + 0xD8, &idle_flag, sizeof(idle_flag)) ||
                !std::isfinite(timer) || std::abs(timer) > 10000 ||
                tx < -10000 || tx > 10000 || ty < -10000 || ty > 10000)
                continue;
            if (config().house_action_sync) {
                char action[64] = {};
                const bool observed = active &&
                    mem_read_std_string((uint8_t*)anim + 0x190, action, sizeof(action)) &&
                    short_house_action(action, state);
                auto it = last_action.find(key);
                if (!observed) {
                    if (it != last_action.end()) last_action.erase(it);
                } else if (it == last_action.end() || it->second != action) {
                    ChoiceMsg a{};
                    a.kind = kChoiceHouseAction; a.cat = key; a.index = state; a.aux = place;
                    strncpy_s(a.name, action, _TRUNCATE);
                    if (net_send_choice(a)) {
                        last_action[key] = action;
                        log_line("CATACTION", "-> cat %016llx %s", (unsigned long long)key, action);
                    }
                }
            }
            ChoiceMsg m{};
            m.kind = kChoiceHouseWalk; m.cat = key; m.aux = place;
            memcpy(&m.index, &fx, sizeof(fx)); memcpy(&m.count, &fy, sizeof(fy));
            m.node_seed = (uint64_t)(uint32_t)tx | ((uint64_t)(uint32_t)ty << 32);
            snprintf(m.name, sizeof(m.name), "%u %.3f %u %u", state, timer,
                     (active || queued) ? 1u : 0u, (unsigned)idle_flag);
            net_send_choice(m);
        }
        return;
    }
    if (!g_cs_ready) return;
    std::map<uint64_t, WalkSample> samples;
    std::map<uint64_t, HouseAction> actions;
    EnterCriticalSection(&g_cs);
    samples.swap(g_walk_pending);
    actions.swap(g_action_pending);
    LeaveCriticalSection(&g_cs);
    if (now < g_walk_pause_until) {
        if (config().house_positions)
            for (const auto& kv : actions)
                log_line("CATACTION", "<- cat %016llx %s skipped: local drag cooldown",
                         (unsigned long long)kv.second.key, kv.second.name);
        return;
    }
    for (const auto& kv : actions) {
        const HouseAction& a = kv.second;
        const char* reason = nullptr;
        if (!config().house_action_sync) reason = "disabled";
        else if (now - a.at > 1000) reason = "expired";
        else if (a.place >= places.n) reason = "room index";
        else if (!short_house_action(a.name, a.state)) reason = "unsupported action";
        void* expected = item(places, a.place);
        uintptr_t vt = 0;
        if (!reason && (!expected || !mem_read(expected, &vt, sizeof(vt)) || vt != g_base + kRva_GridVtbl))
            reason = "room unavailable";
        uint32_t state = 0;
        uint8_t active = 0, queued = 0;
        bool found = false;
        for (uint32_t i = 0; i < cats.n; ++i) {
            if (reason) break;
            void* cat = item(cats, i); void* holder = nullptr; void* anim = nullptr;
            uint64_t key = 0;
            if (!cat || !mem_read((uint8_t*)cat + kHouseCat_Key, &key, sizeof(key)) || key != a.key) continue;
            found = true;
            if (!mem_read((uint8_t*)cat + kHouseCat_Place, &holder, sizeof(holder)) ||
                !mem_read((uint8_t*)cat + 0x94, &state, sizeof(state)) ||
                !mem_read((uint8_t*)cat + 0x68, &anim, sizeof(anim)) || !anim ||
                !mem_read((uint8_t*)anim + 0x210, &active, 1) ||
                !mem_read((uint8_t*)anim + 0x2A0, &queued, 1)) {
                reason = "cat data unreadable";
                break;
            }
            if (holder != expected) { reason = "different room or dragging"; break; }
            if (state != a.state) { reason = "different state"; break; }
            if (active || queued) { reason = "animation busy"; break; }
            const bool ok = safe_short_action(anim, a.name);
            log_line("CATACTION", ok ? "<- cat %016llx %s replayed"
                                     : "!! cat %016llx %s could not be replayed",
                     (unsigned long long)a.key, a.name);
            break;
        }
        if (!found && !reason) reason = "cat missing";
        if (reason && config().house_positions)
            log_line("CATACTION", "<- cat %016llx %s skipped: %s (state host=%u local=%u active=%u queued=%u age=%llums)",
                     (unsigned long long)a.key, a.name, reason, a.state, state, (unsigned)active,
                     (unsigned)queued, (unsigned long long)(now - a.at));
    }
    for (const auto& kv : samples) {
        const WalkSample& s = kv.second;
        // The game's animation callbacks can carry jump/landing state. Do not
        // change a cat while the host has an action or transition in flight.
        if (s.animation_busy || now - s.at > 1500 || s.place >= places.n) continue;
        float fx = 0, fy = 0;
        memcpy(&fx, &s.x, sizeof(fx)); memcpy(&fy, &s.y, sizeof(fy));
        if (!std::isfinite(fx) || !std::isfinite(fy) || std::abs(fx) > 10000 || std::abs(fy) > 10000) continue;
        void* expected = item(places, s.place);
        uintptr_t vt = 0;
        if (!expected || !mem_read(expected, &vt, sizeof(vt)) || vt != g_base + kRva_GridVtbl) continue;
        for (uint32_t i = 0; i < cats.n; ++i) {
            void* cat = item(cats, i); void* holder = nullptr; void* transform = nullptr; void* anim = nullptr;
            uint64_t key = 0;
            uint8_t active = 0, queued = 0;
            if (!cat || !mem_read((uint8_t*)cat + kHouseCat_Key, &key, sizeof(key)) || key != s.key ||
                !mem_read((uint8_t*)cat + kHouseCat_Place, &holder, sizeof(holder)) || holder != expected ||
                !mem_read((uint8_t*)cat + 0x40, &transform, sizeof(transform)) || !transform ||
                !mem_read((uint8_t*)cat + 0x68, &anim, sizeof(anim)) || !anim ||
                !mem_read((uint8_t*)anim + 0x210, &active, sizeof(active)) ||
                !mem_read((uint8_t*)anim + 0x2A0, &queued, sizeof(queued)) || active || queued) continue;
            // Only the observed stationary states and the walking state are
            // candidates. Other states can involve callbacks or transitions
            // that have not been investigated.
            uint32_t local = 0;
            if (!mem_read((uint8_t*)cat + 0x94, &local, sizeof(local))) break;
            // State 2 is standing idle. Try matching it only against another
            // stationary state (4..6), never interrupt a moving cat for it.
            const bool host_safe = (s.state >= 4 && s.state <= 7) ||
                                   (s.state == 2 && local >= 4 && local <= 6);
            const bool local_safe = (local >= 4 && local <= 7) ||
                                    (local == 2 && s.state >= 4 && s.state <= 6);
            if (!host_safe || !local_safe) break;
            if (s.state == 7) {
                mem_write((uint8_t*)cat + 0xF0, &s.target_x, sizeof(s.target_x));
                mem_write((uint8_t*)cat + 0xF4, &s.target_y, sizeof(s.target_y));
            }
            if (local != s.state) {
                if (s.state == 2)
                    mem_write((uint8_t*)cat + 0xD8, &s.idle_flag, sizeof(s.idle_flag));
                mem_write((uint8_t*)cat + 0xC0, &s.timer, sizeof(s.timer));
                mem_write((uint8_t*)cat + 0x94, &s.state, sizeof(s.state));
            }
            // Never slide a stationary or sleeping cat across the floor.
            // Correct only when both peers were already walking; the other
            // states can stay out of position until the next walk.
            double x = 0, y = 0;
            if (s.state == 7 && local == 7 &&
                mem_read((uint8_t*)transform + 0x80, &x, sizeof(x)) &&
                mem_read((uint8_t*)transform + 0x88, &y, sizeof(y)) &&
                std::isfinite(x) && std::isfinite(y)) {
                x += ((double)fx - x) * 0.3;
                y += ((double)fy - y) * 0.3;
                mem_write((uint8_t*)transform + 0x80, &x, sizeof(x));
                mem_write((uint8_t*)transform + 0x88, &y, sizeof(y));
            }
            break;
        }
    }
}

void live_rows_now(std::vector<Row>& rows);
std::vector<Row> g_live_cache;      // the last snapshot taken in a House update
uint64_t         g_live_cache_at = 0;

// From the game now, or -- when this House has not updated for a while (the
// window down, a screen over it: 2026-11-15, 58 rows "only the host has") --
// the last snapshot, which nothing can have changed since.
void live_rows(std::vector<Row>& rows) {
    if (g_comps && GetTickCount64() - g_comps_at <= 2000) { live_rows_now(rows); return; }
    if (g_live_cache_at) {
        log_line("HOUSE", "this House has not updated for %llu ms -- its last snapshot is compared",
                 (unsigned long long)(GetTickCount64() - g_live_cache_at));
        rows.insert(rows.end(), g_live_cache.begin(), g_live_cache.end());
        return;
    }
    live_rows_now(rows);
}

void live_rows_now(std::vector<Row>& rows) {
    if (!g_comps || GetTickCount64() - g_comps_at > 2000) {
        log_line("HOUSE", "!! no House update lately -- where the cats are now is not compared");
        return;
    }
    List places, cats;
    if (!list_of(g_comps, kComps_Places, places) || !list_of(g_comps, kComps_Cats, cats)) {
        log_line("HOUSE", "!! this House's lists did not read -- where the cats are now is not compared");
        return;
    }
    uint32_t added = 0;
    for (uint32_t i = 0; i < cats.n; ++i) {
        void* c = item(cats, i);
        uint64_t key = 0;
        void* at = nullptr;
        if (!c || !mem_read((const uint8_t*)c + kHouseCat_Key, &key, sizeof(key)) || !key ||
            !mem_read((const uint8_t*)c + kHouseCat_Place, &at, sizeof(at)))
            continue;
        Row r;
        r.id = "live_cat " + std::to_string(key);
        if (!at) {
            r.hash  = 0;
            r.shown = "outside";
        } else {
            uint32_t idx = 0xFFFF;
            for (uint32_t j = 0; j < places.n; ++j) if (item(places, j) == at) { idx = j; break; }
            r.hash  = (uint64_t)idx + 1;
            r.shown = idx == 0xFFFF ? "an unlisted place" : std::string(kind_name(kind_of(at))) + " " + std::to_string(idx);
        }
        rows.push_back(std::move(r));
        ++added;
    }
    char n[64];
    snprintf(n, sizeof(n), "%u of %u", added, cats.n);
    Row total;
    total.id    = "live_cats count";
    total.hash  = added;
    total.shown = n;
    rows.push_back(std::move(total));

    {
        void* st = nullptr;
        int32_t coins = 0, food = 0;
        if (mem_read((const void*)(g_base + kRva_FurnitureStore), &st, sizeof(st)) && st &&
            mem_read((const uint8_t*)st + 0xB4, &coins, 4) && mem_read((const uint8_t*)st + 0xB0, &food, 4)) {
            Row c; c.id = "live_purse coins"; c.hash = (uint32_t)coins; c.shown = std::to_string(coins);
            Row f; f.id = "live_purse food";  f.hash = (uint32_t)food;  f.shown = std::to_string(food);
            rows.push_back(std::move(c));
            rows.push_back(std::move(f));
        }
        const uint64_t inv = invsync_local_hash();
        if (inv) {
            char hx[24];
            snprintf(hx, sizeof(hx), "%016llx", (unsigned long long)inv);
            Row r; r.id = "live_inventory hash"; r.hash = inv; r.shown = hx;
            rows.push_back(std::move(r));
        }
    }
    if (void* w = weather_str()) {
        char wx[64] = {};
        if (mem_read_std_string(w, wx, sizeof(wx))) {
            Row r;
            r.id    = "live_weather now";
            r.shown = wx;
            r.hash  = fnv(0xCBF29CE484222325ull, wx, strlen(wx));
            rows.push_back(std::move(r));
        }
    }

    // THE FURNITURE AS IT IS NOW, poops and all. The game's furniture store
    // (the pointer at 0x1413DA9A0, as the poop's clean-up reads it): records
    // at +0x40, their count at +0x3C, each record 0x68 bytes -- its id at +0
    // (the save's key), its name (std::string) at +8, its room at +0x30. The
    // friend's save is written only now and then, and a poop cleaned there a
    // few seconds before the day's end was still in it (2026-11-15).
    void* store = nullptr;
    uint32_t fn = 0;
    void** recs = nullptr;
    if (!mem_read((const void*)(g_base + kRva_FurnitureStore), &store, sizeof(store)) || !store ||
        !mem_read((const uint8_t*)store + 0x3C, &fn, sizeof(fn)) ||
        !mem_read((const uint8_t*)store + 0x40, &recs, sizeof(recs)) || !recs || fn > 4096) {
        log_line("HOUSE", "!! the furniture store did not read -- the furniture is compared by the saves");
        return;
    }
    uint32_t got = 0;
    for (uint32_t i = 0; i < fn; ++i) {
        void* r = nullptr;
        uint64_t id = 0;
        char name[64] = {}, room[64] = {};
        if (!mem_read(&recs[i], &r, sizeof(r)) || !r || !mem_read(r, &id, sizeof(id)) ||
            !mem_read_std_string((const uint8_t*)r + 8, name, sizeof(name)) ||
            !mem_read_std_string((const uint8_t*)r + 0x30, room, sizeof(room)))
            continue;
        Row f;
        f.id    = "live_furniture " + std::to_string(id);
        f.shown = std::string(name) + " in " + room;
        f.hash  = fnv(fnv(0xCBF29CE484222325ull, name, strlen(name) + 1), room, strlen(room));
        rows.push_back(std::move(f));
        ++got;
    }
    snprintf(n, sizeof(n), "%u of %u", got, fn);
    Row ft;
    ft.id    = "live_furniture count";
    ft.hash  = got;
    ft.shown = n;
    rows.push_back(std::move(ft));
}

// THE NOTE: "the other player sent X to Y" in the top right corner, for a few
// seconds (the user's ask, 2026-11-15). The NPC by the pick's index, as the
// game names them (its switch at 0x274180: Beanies, Butch, Tink, Frank, Jack,
// Tracy, OrganGrinder, Steven; the logs agree, Tink 2 and Tracy 5), in the
// dative for "to", the Russian names from the game's own combined.csv.
std::string g_note;
uint64_t    g_note_at = 0;
constexpr uint64_t kNoteMs = 7000;

void note_pipe(const std::string& cat, uint32_t npc) {
    static const char* const kRu[8] = {
        u8"\u041f. \u041e. \u0414\u0443\u0448\u0435\u0447\u043a\u0438\u043d\u0443",   // \u041f. \u041e. \u0414\u0443\u0448\u0435\u0447\u043a\u0438\u043d\u0443
        u8"\u0411\u0443\u0442\u0447\u0443",                                         // \u0411\u0443\u0442\u0447\u0443
        u8"\u0422\u0438\u043d\u043a\u0443",                                         // \u0422\u0438\u043d\u043a\u0443
        u8"\u0424\u0440\u044d\u043d\u043a\u0443",                                   // \u0424\u0440\u044d\u043d\u043a\u0443
        u8"\u041c\u0430\u043b\u044b\u0448\u0443 \u0414\u0436\u0435\u043a\u0443",     // \u041c\u0430\u043b\u044b\u0448\u0443 \u0414\u0436\u0435\u043a\u0443
        u8"\u0422\u0440\u0435\u0439\u0441\u0438",                                   // \u0422\u0440\u0435\u0439\u0441\u0438
        u8"\u041e\u0440\u0433\u0430\u043d\u043e\u0438\u0434\u0443",                 // \u041e\u0440\u0433\u0430\u043d\u043e\u0438\u0434\u0443
        u8"\u0421\u0442\u0438\u0432\u0435\u043d\u0443",                             // \u0421\u0442\u0438\u0432\u0435\u043d\u0443
    };
    static const char* const kEn[8] = { "Dr. Beanies", "Butch", "Tink", "Frank", "Baby Jack", "Tracy",
                                        "the Organ Grinder", "Steven" };
    if (npc >= 8) return;
    g_note = std::string(tr(u8"\u0414\u0440\u0443\u0433\u043e\u0439 \u0438\u0433\u0440\u043e\u043a \u043e\u0442\u043f\u0440\u0430\u0432\u0438\u043b \u00ab",
                            "Another player sent \"")) +
             cat + tr(u8"\u00bb \u043a ", "\" to ") + tr(kRu[npc], kEn[npc]);
    g_note_at = GetTickCount64();
}

// The other player's pick, from the drawer's own update (the component
// house_tick is given is the NPC map drawer). The cat is in this game's pipe
// and its map up by now; the same NPC's lambda runs on it.
void deliver_here(void* drawer, void* comps, const Move& mv) {
    List cats;
    list_of(comps, kComps_Cats, cats);
    void* cat = nullptr;
    for (uint32_t j = 0; j < cats.n && !cat; ++j) {
        void* c = item(cats, j);
        uint64_t k = 0;
        if (c && mem_read((const uint8_t*)c + kHouseCat_Key, &k, sizeof(k)) && k == mv.key) cat = c;
    }
    const std::string name = night_cat_name(mv.key);
    if (!cat || mv.index >= 8 || !o_pick[mv.index]) {
        log_line("HOUSE", "!! %s (%016llx) is not in this House -- not sent down the pipe here",
                 name.c_str(), (unsigned long long)mv.key);
        return;
    }
    void* had = nullptr;
    uint8_t on = 0;
    mem_read((const uint8_t*)drawer + kDrawer_PipeCat, &had, sizeof(had));
    mem_read((const uint8_t*)drawer + kDrawer_PipeOn, &on, 1);
    if (had != cat) mem_write((uint8_t*)drawer + kDrawer_PipeCat, &cat, sizeof(cat));
    g_delivering = true;
    const bool ok = safe_pick(mv.index, drawer);
    g_delivering = false;
    g_pipe_remote_key = 0;
    // The map closes after a click of its own: on the picking screen (the
    // 2026-11-15 watch) the pipe mode +0x40 is down by then and +0xC0, a double,
    // starts at 0.2333 and runs to 0 over a quarter second, when the drawer's
    // update (0x2786C3) closes the map. The same here.
    const uint8_t zero = 0;
    const double  closing = 0.23333333333333334;
    mem_write((uint8_t*)drawer + kDrawer_PipeOn, &zero, 1);
    mem_write((uint8_t*)drawer + 0xC0, &closing, sizeof(closing));
    if (ok) note_pipe(name, mv.index);
    log_line("HOUSE", ok ? "<- %s (%016llx) sent down the pipe to NPC %u, as the other player picked (this map had %s)"
                         : "!! sending %s (%016llx) down the pipe to NPC %u faulted (this map had %s)",
             name.c_str(), (unsigned long long)mv.key, mv.index,
             had == cat && on ? "it up" : had ? "another cat" : "no cat");
}

} // namespace

const char* house_note() {
    if (g_note.empty() || GetTickCount64() - g_note_at > kNoteMs) return nullptr;
    return g_note.c_str();
}

void house_set_base(uintptr_t base) {
    if (o_drop) return;
    g_base = base;
    {
        void** slot = (void**)(base + kRva_GridVtbl + 34 * 8);
        void* have = nullptr;
        DWORD prot = 0;
        if (mem_read(slot, &have, sizeof(have)) && (uintptr_t)have == base + kRva_PoopSpawn &&
            VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &prot)) {
            o_poop_spawn = (fn_spawn)have;
            *slot = (void*)&poop_spawn;
            VirtualProtect(slot, sizeof(void*), prot, &prot);
        } else {
            log_line("HOUSE", "!! the cats' poop moved -- each game makes its own by day");
        }
    }
    {
        void* at = (void*)(base + kRva_InterOk);
        if (memcmp(at, kInterOkHead, sizeof(kInterOkHead)) != 0 ||
            MH_CreateHook(at, (void*)&inter_ok, (void**)&o_inter_ok) != MH_OK || MH_EnableHook(at) != MH_OK) {
            o_inter_ok = nullptr;
            log_line("HOUSE", "!! the night screen's OK moved -- each player turns their own night's screens");
        }
    }
    {
        uint32_t hooked = 0;
        for (uint32_t i = 0; i < 8; ++i) {
            void* at = (void*)(base + kPickRva[i]);
            if (memcmp(at, kPickHead, sizeof(kPickHead)) == 0 &&
                MH_CreateHook(at, kPickDetour[i], (void**)&o_pick[i]) == MH_OK && MH_EnableHook(at) == MH_OK)
                ++hooked;
            else
                o_pick[i] = nullptr;
        }
        o_deliver_ok = hooked == 8;
        if (!o_deliver_ok)
            log_line("HOUSE", "!! %u of the NPC map's 8 picks hooked -- the other player picks for themself", hooked);
    }
    {
        void** slot = (void**)(base + kRva_PipeVtbl + 31 * 8);
        void* have = nullptr;
        DWORD prot = 0;
        if (mem_read(slot, &have, sizeof(have)) && (uintptr_t)have == base + kRva_PipeTake &&
            VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &prot)) {
            o_pipe_take = (fn_take)have;
            *slot = (void*)&pipe_take;
            VirtualProtect(slot, sizeof(void*), prot, &prot);
        } else {
            log_line("HOUSE", "!! the pipe's take moved -- a cat sent down the pipe goes to the yard on the other screen");
        }
    }
    {
        void** slot = (void**)(base + kRva_FurnUIVtbl + 10 * 8);
        void* have = nullptr;
        DWORD prot = 0;
        if (mem_read(slot, &have, sizeof(have)) && (uintptr_t)have == base + kRva_FurnUIUpdate &&
            VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &prot)) {
            o_furn_ui = (fn_upd2)have;
            *slot = (void*)&furn_ui_update;
            VirtualProtect(slot, sizeof(void*), prot, &prot);
        } else {
            log_line("HOUSE", "!! the furniture panel moved -- its list shows the other player's changes on reopening");
        }
    }
    {
        struct { uintptr_t rva; const char* head; size_t n; void* det; void** orig; const char* what; } k[] = {
            { 0x205B00, "\x48\x89\x5c\x24\x08\x48\x89\x6c\x24\x18", 10, (void*)&rec_add, (void**)&o_rec_add, "record add" },
            { 0x205D50, "\x40\x57\x48\x83\xec\x20\x44\x8b\x51\x3c", 10, (void*)&rec_drop, (void**)&o_rec_drop, "record drop" },
            { 0x2EC7A0, "\x48\x89\x5c\x24\x18\x55\x56\x57\x41\x54", 10, (void*)&piece_init, (void**)&o_piece_init, "piece init" },
        };
        for (auto& h : k) {
            void* at = (void*)(base + h.rva);
            if (memcmp(at, h.head, h.n) != 0 || MH_CreateHook(at, h.det, h.orig) != MH_OK || MH_EnableHook(at) != MH_OK)
                log_line("FURNWATCH", "!! %s did not hook", h.what);
        }
    }
    {
        void* at = (void*)(base + kRva_PoopClean);
        if (memcmp(at, kPoopCleanHead, sizeof(kPoopCleanHead)) != 0 ||
            MH_CreateHook(at, (void*)&poop_clean, (void**)&o_poop_clean) != MH_OK ||
            MH_EnableHook(at) != MH_OK) {
            o_poop_clean = nullptr;
            log_line("HOUSE", "!! the poop's clean-up moved -- a poop cleaned on one screen stays on the other");
        }
    }
    {
        void** slot = (void**)(base + kRva_InterVtbl + 10 * 8);
        void* have = nullptr;
        DWORD prot = 0;
        if (mem_read(slot, &have, sizeof(have)) && (uintptr_t)have == base + kRva_InterUpdate &&
            VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &prot)) {
            o_inter_update = (fn_upd)have;
            *slot = (void*)&inter_update;
            VirtualProtect(slot, sizeof(void*), prot, &prot);
        } else {
            log_line("HOUSE", "!! the night's event screen moved -- the morning is sent at its first write");
        }
    }
    {
        void* at = (void*)(base + kRva_Resolve);
        if (memcmp((const void*)(base + kRva_EndTheDay), kEndTheDayHead, sizeof(kEndTheDayHead)) == 0 &&
            memcmp(at, kResolveHead, sizeof(kResolveHead)) == 0 &&
            MH_CreateHook(at, (void*)&resolve, (void**)&o_resolve) == MH_OK && MH_EnableHook(at) == MH_OK) {
            end_the_day = (fn_house)(base + kRva_EndTheDay);
        } else {
            o_resolve = nullptr;
            log_line("HOUSE", "!! the night's resolver moved -- the friend's game reloads into each morning");
        }
    }
    if (!g_cs_ready) { InitializeCriticalSection(&g_cs); g_cs_ready = true; }
    void* at = (void*)(base + kRva_Drop);
    if (memcmp((const void*)(base + kRva_PlaceAdd), kPlaceAddHead, 8) != 0 ||
        memcmp(at, kDropHead, sizeof(kDropHead)) != 0 ||
        MH_CreateHook(at, (void*)&drop, (void**)&o_drop) != MH_OK || MH_EnableHook(at) != MH_OK) {
        o_drop = nullptr;
        log_line("HOUSE", "!! the House's cat drop moved -- cats moved in the House stay on each screen");
        return;
    }
    place_add = (fn_place)(base + kRva_PlaceAdd);
    if (memcmp((const void*)(base + kRva_PlaceRemove), kPlaceRemoveHead, 8) == 0)
        place_remove = (fn_place)(base + kRva_PlaceRemove);
    else
        log_line("HOUSE", "!! the place's take-out moved -- a cat put outside stays in its room on the other screen");
    drag_install();
}

bool house_on_choice(const ChoiceMsg& m, uint8_t from) {
    if (m.kind == kChoiceHouseDragTrace) {
        if ((!config().house_drag_trace && !config().house_drag_sync) ||
            !g_cs_ready || !m.cat || !m.node_seed || m.index > 1 ||
            from == kNoPeer || from == net_self()) return true;
        EnterCriticalSection(&g_cs);
        if (g_drag_messages.size() < 256) g_drag_messages.push_back(DragMessage{m, from});
        LeaveCriticalSection(&g_cs);
        return true;
    }
    if (m.kind == kChoiceHouseAction) {
        if (config().house_walk_sync && config().house_action_sync &&
            net_role() == NetRole::Client && g_cs_ready && m.cat &&
            short_house_action(m.name, m.index)) {
            HouseAction a{};
            a.key = m.cat; a.at = GetTickCount64(); a.place = m.aux; a.state = m.index;
            strncpy_s(a.name, m.name, _TRUNCATE);
            EnterCriticalSection(&g_cs);
            if (g_action_pending.size() < 256 || g_action_pending.count(m.cat))
                g_action_pending[m.cat] = a;
            LeaveCriticalSection(&g_cs);
        }
        return true;
    }
    if (m.kind == kChoiceHouseWalk) {
        if (config().house_walk_sync && net_role() == NetRole::Client && g_cs_ready && m.cat) {
            unsigned state = 0;
            unsigned busy = 0;
            unsigned idle_flag = 0;
            double timer = 0;
            const int32_t tx = (int32_t)(uint32_t)m.node_seed;
            const int32_t ty = (int32_t)(uint32_t)(m.node_seed >> 32);
            if (sscanf_s(m.name, "%u %lf %u %u", &state, &timer, &busy, &idle_flag) != 4 ||
                state > 7 || busy > 1 || idle_flag > 1 || !std::isfinite(timer) || std::abs(timer) > 10000 ||
                tx < -10000 || tx > 10000 || ty < -10000 || ty > 10000)
                return true;
            EnterCriticalSection(&g_cs);
            if (g_walk_pending.size() < 256 || g_walk_pending.count(m.cat))
                g_walk_pending[m.cat] = WalkSample{m.cat, GetTickCount64(), m.aux, m.index, m.count,
                                                   state, tx, ty, timer, busy != 0, (uint8_t)idle_flag};
            LeaveCriticalSection(&g_cs);
        }
        return true;
    }
    if (m.kind == kChoiceHousePurse) {
        // NPC offices follow the host's actions; the client's local shop
        // result can differ and must not overwrite the host's balance.
        if (net_role() == NetRole::Host && cutscene_office_active()) {
            log_line("HOUSE", "ignored the friend's purse during the host's NPC office");
            return true;
        }
        apply_purse((int32_t)m.index, (int32_t)m.count);
        return true;
    }
    if (m.kind == kChoiceNightClick) {
        if (net_role() == NetRole::Host || !g_cs_ready) return true;
        EnterCriticalSection(&g_cs);
        if (g_click_n < 16) g_clicks[g_click_n++] = NightClick{ m.index, m.aux, m.count };
        LeaveCriticalSection(&g_cs);
        return true;
    }
    if (m.kind == kChoicePipeSend) {
        if (!g_cs_ready || !o_deliver_ok) return true;
        // In the moves' queue, behind the cat's move into the pipe: on
        // 2026-11-15 it was handed over first, with the cat still in the yard.
        EnterCriticalSection(&g_cs);
        if (g_move_n < 32) g_moves[g_move_n++] = Move{ m.cat, m.index, 0, kMoveDeliver, GetTickCount64() };
        LeaveCriticalSection(&g_cs);
        return true;
    }
    if (m.kind == kChoiceFurniture) {
        if (!g_cs_ready) return true;
        EnterCriticalSection(&g_cs);
        if (g_furn_ops.size() < 256)
            g_furn_ops.push_back(FurnOp{ m.cat, m.index, m.name, (int32_t)(uint32_t)m.node_seed,
                                         (int32_t)(uint32_t)(m.node_seed >> 32) });
        LeaveCriticalSection(&g_cs);
        return true;
    }
    if (m.kind == kChoicePoopClean) {
        if (!g_cs_ready || !o_poop_clean) return true;
        EnterCriticalSection(&g_cs);
        if (g_poop_n < 32) g_poops[g_poop_n++] = m.cat;
        LeaveCriticalSection(&g_cs);
        return true;
    }
    if (m.kind == kChoiceNightStart) {
        if (net_role() == NetRole::Host) return true;
        unsigned long long w2 = 0, w3 = 0;
        if (sscanf_s(m.name, "%16llx%16llx", &w2, &w3) != 2) return true;
        const uint64_t now = GetTickCount64();
        if (!end_the_day || !o_resolve) {
            log_line("HOUSE", "<- the host's night begins -- this game cannot resolve one, it reloads into the morning");
            return true;
        }
        if (!g_match || now - g_match_at > kMatchFreshMs) {
            log_line("HOUSE", "<- the host's night begins -- the Houses were not the same, so this game"
                              " reloads into the host's morning as before");
            return true;
        }
        g_seed[0] = m.node_seed;
        g_seed[1] = m.cat;
        g_seed[2] = w2;
        g_seed[3] = w3;
        g_seed_at  = now;
        g_night_go = now;
        log_line("HOUSE", "<- the host's night begins -- this House is the host's, so this game"
                          " ends its own day and resolves the same night");
        return true;
    }
    if (m.kind != kChoiceHouseMove) return false;
    if (!g_cs_ready || !place_add) return true;
    EnterCriticalSection(&g_cs);
    if (g_move_n < 32)
        g_moves[g_move_n++] = Move{ m.cat, m.index, m.count, m.aux, GetTickCount64(), from, m.node_seed };
    LeaveCriticalSection(&g_cs);
    return true;
}

bool house_pipe_delivery_pending() {
    if (!g_cs_ready) return false;
    bool pending = false;
    EnterCriticalSection(&g_cs);
    for (uint32_t i = 0; i < g_move_n; ++i)
        if (g_moves[i].kind == kMoveDeliver) { pending = true; break; }
    LeaveCriticalSection(&g_cs);
    return pending;
}

bool house_drag_remote_held(void* cat) {
    if (!remote_drag_cat(cat)) return false;
    drag_guard_note(cat, true);
    return true;
}
void house_drag_remote_placed(void* cat) { finish_remote_drag(cat, false); }

void house_tick(void* component) {
    void* comps = nullptr;
    if (!comps_of(component, comps)) return;
    g_comps    = comps;
    g_comps_at = GetTickCount64();
    drag_tick(comps);
    trace_house_drag(comps);
    trace_cat_positions(comps);
    walk_tick(comps);
    if (g_comps_at - g_live_cache_at > 1000) {
        g_live_cache.clear();
        live_rows_now(g_live_cache);
        g_live_cache_at = g_comps_at;
    }
    watch_drawer(component);
    if (!g_furn_ops.empty() && g_cs_ready && leave_on_house()) {
        std::vector<FurnOp> ops;
        EnterCriticalSection(&g_cs);
        ops.swap(g_furn_ops);
        LeaveCriticalSection(&g_cs);
        void* owner = nullptr; void* scene = nullptr;
        if (mem_read((const uint8_t*)component + kComp_Owner, &owner, sizeof(owner)) && owner &&
            mem_read((const uint8_t*)owner + kOwner_Scene, &scene, sizeof(scene)) && scene)
            for (const FurnOp& op : ops) apply_furniture(comps, scene, op);
    }
    furniture_watch(comps);
    if (g_night_go) {
        const char* step0 = nullptr;
        if (GetTickCount64() - g_night_go > kNightGoMs) {
            g_night_go = 0;
            g_seed_at  = 0;
            log_line("HOUSE", "!! this game's House did not come up in time -- it reloads into the host's morning");
        } else if (leave_on_house() && !leave_loading(&step0)) {
            g_night_go = 0;
            List hl;
            void* house = nullptr;
            uintptr_t vt = 0;
            uint8_t ends = 1;
            if (list_of(comps, kComps_House, hl)) house = item(hl, 0);
            if (!house || !mem_read(house, &vt, sizeof(vt)) || vt != g_base + kRva_HouseVtbl ||
                !mem_read((const uint8_t*)house + kHouse_DayEnds, &ends, 1)) {
                g_seed_at = 0;
                log_line("HOUSE", "!! this game's House did not read -- it reloads into the host's morning");
            } else if (ends) {
                g_seed_at = 0;
                log_line("HOUSE", "!! this game's day was already ending -- it reloads into the host's morning");
            } else if (!safe_end_the_day(house)) {
                g_seed_at = 0;
                log_line("HOUSE", "!! ending this game's day faulted -- it reloads into the host's morning");
            } else {
                g_local_at = GetTickCount64();
                g_night_at = g_local_at;
                log_line("HOUSE", "this game's day ends with the host's");
            }
        }
    }
    if (g_poop_n && leave_on_house()) {
        uint64_t ids[32];
        uint32_t n = 0;
        EnterCriticalSection(&g_cs);
        n = g_poop_n;
        memcpy(ids, g_poops, n * sizeof(uint64_t));
        g_poop_n = 0;
        LeaveCriticalSection(&g_cs);
        // The pieces' list: the component list whose items are FurniturePieces.
        List pieces;
        if (g_pieces_at) {
            void* first = nullptr;
            uintptr_t vt = 0;
            if (!list_of(comps, g_pieces_at, pieces) || !(first = item(pieces, 0)) ||
                !mem_read(first, &vt, sizeof(vt)) || vt != g_base + kRva_PieceVtbl)
                g_pieces_at = 0;
        }
        for (uintptr_t at = 0x10; !g_pieces_at && at < 0x8000; at += 0x10) {
            List l;
            void* first = nullptr;
            uintptr_t vt = 0;
            if (list_of(comps, at, l) && (first = item(l, 0)) && mem_read(first, &vt, sizeof(vt)) &&
                vt == g_base + kRva_PieceVtbl) {
                g_pieces_at = at;
                pieces = l;
            }
        }
        for (uint32_t i = 0; i < n; ++i) {
            void* found = nullptr;
            for (uint32_t j = 0; g_pieces_at && j < pieces.n && !found; ++j) {
                void* p = item(pieces, j);
                uint64_t id = 0;
                void* kind = nullptr;
                if (p && mem_read((const uint8_t*)p + kPiece_Id, &id, sizeof(id)) && id == ids[i] &&
                    mem_read((const uint8_t*)p + kPiece_Kind, &kind, sizeof(kind)) && kind)
                    found = p;
            }
            if (!found) {
                log_line("HOUSE", "!! poop %llu is not in this House -- not cleaned here", (unsigned long long)ids[i]);
                continue;
            }
            g_cleaning = true;
            const bool ok = safe_poop_clean(found);
            g_cleaning = false;
            log_line("HOUSE", ok ? "<- poop %llu cleaned up, as the other player did"
                                 : "!! cleaning poop %llu faulted", (unsigned long long)ids[i]);
        }
    }
    if (!g_cs_ready || !g_move_n) return;
    const char* step = nullptr;
    if (!leave_on_house() || leave_loading(&step)) return;
    const uint64_t now = GetTickCount64();
    Move todo[32];
    uint32_t n = 0;
    EnterCriticalSection(&g_cs);
    uint32_t keep = 0;
    for (uint32_t i = 0; i < g_move_n; ++i) {
        // In order: one not settled yet holds back the ones after it.
        const uint64_t settle = g_moves[i].kind == kMoveDeliver ? kDeliverSettleMs : kMoveSettleMs;
        if (keep || now - g_moves[i].at < settle) {
            if (now - g_moves[i].at > 30000) continue;   // never came to a House: dropped
            g_moves[keep++] = g_moves[i];
        } else {
            todo[n++] = g_moves[i];
        }
    }
    g_move_n = keep;
    LeaveCriticalSection(&g_cs);
    for (uint32_t i = 0; i < n; ++i) {
        if (todo[i].kind == kMoveDeliver) deliver_here(component, comps, todo[i]);
        else apply(comps, todo[i]);
    }
}

namespace {
// ---------------------------------------------------------------------------
// THE FURNITURE (the user's ask, 2026-11-15). The store's record per piece:
// id +0, kind +8, room +0x30 ("" in storage), tile x +0x50 and y +0x54 (int32),
// draw order +0x60 (renumbered by the game). What the 2026-11-15 watch showed:
//   bought      the shop adds a record (sub_140205B00(store, kind, 0)) in storage
//   picked up   the room goes "" and the piece is destroyed
//   put down    a piece is made from the record (scene's new entity,
//               sub_1401ACA10(scene, entity, &id) -> FurniturePiece init), then
//               the room and tile are set
// Each game's records are polled; a change made here goes to the other player
// as the record's new state, and the other game does what the game does: the
// record set, the old piece destroyed, a new one made from the record (as the
// House makes them all as it loads). Poops are left to the night and to their
// own clean-up.
constexpr uintptr_t kRva_NewEntity   = 0x96B3E0;
constexpr uintptr_t kRva_AddPiece    = 0x1ACA10;
// sub_14094EBA0(component): the component's whole entity marked dead and its
// components with it -- what the House does with a piece it could not place
// (sub_1401E02F0). sub_14096B6A0, the poop's, takes the piece alone and left
// its picture standing on the friend's screen (2026-11-15).
constexpr uintptr_t kRva_Destroy     = 0x94EBA0;
constexpr uintptr_t kRva_StrAssign   = 0x520D0;
constexpr uintptr_t kRva_FurnCounter = 0x13C1920;
constexpr uintptr_t kScene_Dying     = 0x4B0;
using fn_newent  = void* (__fastcall*)(void*);
using fn_addp    = void* (__fastcall*)(void*, void*, uint64_t*);
using fn_destroy = void  (__fastcall*)(void*);
using fn_sassign = void* (__fastcall*)(void*, const char*, size_t);

struct Placed { std::string name, room; int32_t x = 0, y = 0; };
std::map<uint64_t, Placed> g_known;       // the state both games agree on
bool     g_known_ok = false;
// THE PURSE. The House's coins and food live beside the furniture store's
// records: [0x1413DA9A0]+0xB4 coins, +0xB0 food (the house shop's purchase,
// sub_14079A600, checks +0xB4 and its grant adds to both). A purchase took the
// coins on one screen only, and that night was not shared (2026-11-15).
constexpr uintptr_t kStore_Food  = 0xB0;
constexpr uintptr_t kStore_Coins = 0xB4;
int32_t  g_purse_coins = 0, g_purse_food = 0;

void* store_ptr() {
    void* store = nullptr;
    mem_read((const void*)(g_base + kRva_FurnitureStore), &store, sizeof(store));
    return store;
}

bool read_records(std::map<uint64_t, Placed>& out, std::map<uint64_t, void*>* where = nullptr) {
    void* store = store_ptr();
    uint32_t fn = 0;
    void** recs = nullptr;
    if (!store || !mem_read((const uint8_t*)store + 0x3C, &fn, sizeof(fn)) ||
        !mem_read((const uint8_t*)store + 0x40, &recs, sizeof(recs)) || !recs || fn > 4096)
        return false;
    for (uint32_t i = 0; i < fn; ++i) {
        void* r = nullptr;
        uint64_t id = 0;
        char name[64] = {}, room[64] = {};
        Placed p;
        if (!mem_read(&recs[i], &r, sizeof(r)) || !r || !mem_read(r, &id, sizeof(id)) ||
            !mem_read_std_string((const uint8_t*)r + 8, name, sizeof(name)) ||
            !mem_read_std_string((const uint8_t*)r + 0x30, room, sizeof(room)))
            continue;
        mem_read((const uint8_t*)r + 0x50, &p.x, 4);
        mem_read((const uint8_t*)r + 0x54, &p.y, 4);
        p.name = name;
        p.room = room;
        out[id] = p;
        if (where) (*where)[id] = r;
    }
    return true;
}

void send_furn(uint64_t id, uint32_t op, const std::string& text, int32_t x, int32_t y) {
    ChoiceMsg m{};
    m.kind      = kChoiceFurniture;
    m.cat       = id;
    m.index     = op;
    m.node_seed = (uint64_t)(uint32_t)x | ((uint64_t)(uint32_t)y << 32);
    strncpy_s(m.name, text.c_str(), _TRUNCATE);
    net_send_choice(m);
}

void apply_purse(int32_t coins, int32_t food) {
    void* st = store_ptr();
    if (st && mem_write((uint8_t*)st + kStore_Coins, &coins, 4) && mem_write((uint8_t*)st + kStore_Food, &food, 4)) {
        g_purse_coins = coins;
        g_purse_food  = food;
        log_line("HOUSE", "<- the House's purse: %d coins, %d food, as the other player's", coins, food);
    }
}

void furniture_watch(void* comps) {
    (void)comps;
    const uint64_t now = GetTickCount64();
    if (now - g_watch_at < 250) return;
    g_watch_at = now;
    // Only while both stand in the House with the day under way; a night
    // (poops) or a reload starts the agreed state afresh.
    if (!net_active() || net_peer_count() < 2 || !leave_on_house() || g_night_at || g_night_go) {
        g_known_ok = false;
        return;
    }
    std::map<uint64_t, Placed> cur;
    if (!read_records(cur)) return;
    void* st = store_ptr();
    int32_t coins = 0, food = 0;
    const bool purse = st && mem_read((const uint8_t*)st + kStore_Coins, &coins, 4) &&
                       mem_read((const uint8_t*)st + kStore_Food, &food, 4);
    if (!g_known_ok) {
        g_known.swap(cur);
        g_known_ok = true;
        g_purse_coins = coins;
        g_purse_food  = food;
        return;
    }
    if (purse && (coins != g_purse_coins || food != g_purse_food)) {
        if (net_role() != NetRole::Host && cutscene_office_active()) {
            // The office runs on both games, but only the host's result is
            // authoritative. Remember this observation without echoing it.
            g_purse_coins = coins;
            g_purse_food = food;
        } else {
        ChoiceMsg m{};
        m.kind  = kChoiceHousePurse;
        m.index = (uint32_t)coins;
        m.count = (uint32_t)food;
        net_send_choice(m);
        log_line("HOUSE", "-> the House's purse: %d coins (was %d), %d food (was %d)",
                 coins, g_purse_coins, food, g_purse_food);
        g_purse_coins = coins;
        g_purse_food  = food;
        }
    }
    for (auto& kv : cur) {
        const Placed& p = kv.second;
        auto it = g_known.find(kv.first);
        // Poops by day are the host's (the client's own are not made, see
        // poop_spawn); the host sends them like furniture.
        const bool poop = p.name == "poop" && net_role() != NetRole::Host;
        if (it == g_known.end()) {
            if (!poop) {
                send_furn(kv.first, 1, p.name, 0, 0);
                log_line("HOUSE", "-> furniture %llu '%s' bought", (unsigned long long)kv.first, p.name.c_str());
                if (!p.room.empty()) send_furn(kv.first, 2, p.room, p.x, p.y);
            }
        } else if (it->second.room != p.room || it->second.x != p.x || it->second.y != p.y) {
            if (!poop) {
                send_furn(kv.first, 2, p.room, p.x, p.y);
                log_line("HOUSE", "-> furniture %llu '%s' %s %s (%d,%d)", (unsigned long long)kv.first, p.name.c_str(),
                         p.room.empty() ? "put away" : "put in", p.room.c_str(), p.x, p.y);
            }
        }
    }
    for (auto& kv : g_known)
        if (!cur.count(kv.first) && kv.second.name != "poop") {   // cleaned: their own message
            send_furn(kv.first, 3, "", 0, 0);
            log_line("HOUSE", "-> furniture %llu '%s' gone", (unsigned long long)kv.first, kv.second.name.c_str());
        }
    g_known.swap(cur);
}

bool safe_new_piece(void* scene, uint64_t id) {
    __try {
        void* ent = ((fn_newent)(g_base + kRva_NewEntity))(scene);
        if (!ent) return false;
        uint64_t k = id;
        return ((fn_addp)(g_base + kRva_AddPiece))(scene, ent, &k) != nullptr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool safe_destroy(void* piece) {
    __try { ((fn_destroy)(g_base + kRva_Destroy))(piece); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool safe_sassign(void* str, const char* text) {
    __try { ((fn_sassign)(g_base + kRva_StrAssign))(str, text, strlen(text)); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool safe_rec_add(void* store, void* str, uint64_t& id) {
    __try { id = o_rec_add(store, str, 0); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool safe_rec_drop(void* store, uint64_t id) {
    __try { o_rec_drop(store, id); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void* piece_of(void* comps, uint64_t id) {
    List pieces;
    if (!g_pieces_at || !list_of(comps, g_pieces_at, pieces)) return nullptr;
    for (uint32_t j = 0; j < pieces.n; ++j) {
        void* p = item(pieces, j);
        uint64_t k = 0;
        void* kind = nullptr;
        if (p && mem_read((const uint8_t*)p + kPiece_Id, &k, sizeof(k)) && k == id &&
            mem_read((const uint8_t*)p + kPiece_Kind, &kind, sizeof(kind)) && kind)
            return p;
    }
    return nullptr;
}

void find_pieces(void* comps) {
    for (uintptr_t at = 0x10; !g_pieces_at && at < 0x8000; at += 0x10) {
        List l;
        void* f0 = nullptr;
        uintptr_t vt = 0;
        if (list_of(comps, at, l) && (f0 = item(l, 0)) && mem_read(f0, &vt, sizeof(vt)) &&
            vt == g_base + kRva_PieceVtbl)
            g_pieces_at = at;
    }
}

void apply_furniture(void* comps, void* scene, const FurnOp& op) {
    void* store = store_ptr();
    if (!store || !o_rec_add || !o_rec_drop) return;
    find_pieces(comps);
    std::map<uint64_t, Placed> recs;
    std::map<uint64_t, void*> where;
    read_records(recs, &where);
    const unsigned long long id = (unsigned long long)op.id;
    g_furn_dirty = true;
    if (op.op == 1) {
        if (where.count(op.id)) return;
        // A std::string the store takes by value (and frees): the game's own
        // assign gives it the game's own buffer.
        alignas(8) uint8_t str[32] = {};
        uint64_t cap = 15;
        memcpy(str + 0x18, &cap, 8);
        uint64_t got = 0;
        if (!safe_sassign(str, op.text.c_str()) || !safe_rec_add(store, str, got)) {
            log_line("HOUSE", "!! furniture %llu '%s' could not be added here", id, op.text.c_str());
            return;
        }
        if (got != op.id) {
            // The ids are the save's keys: this game's counter was elsewhere.
            std::map<uint64_t, void*> w2;
            std::map<uint64_t, Placed> r2;
            read_records(r2, &w2);
            if (w2.count(got)) mem_write(w2[got], &op.id, sizeof(op.id));
            uint64_t counter = 0;
            mem_read((const void*)(g_base + kRva_FurnCounter), &counter, sizeof(counter));
            if (counter < op.id) mem_write((void*)(g_base + kRva_FurnCounter), &op.id, sizeof(op.id));
            log_line("HOUSE", "!! furniture '%s' got id %llu here, the other player's %llu -- renumbered",
                     op.text.c_str(), (unsigned long long)got, id);
        }
        g_known[op.id] = Placed{ op.text, "", 0, 0 };
        log_line("HOUSE", "<- furniture %llu '%s' bought, as the other player did", id, op.text.c_str());
        return;
    }
    if (!where.count(op.id)) {
        log_line("HOUSE", "!! furniture %llu is not in this game's store -- not changed", id);
        return;
    }
    void* rec = where[op.id];
    // Every piece of this record (the 2026-11-15 build left some standing).
    {
        List pieces;
        uint32_t gone = 0;
        if (g_pieces_at && list_of(comps, g_pieces_at, pieces))
            for (uint32_t j = 0; j < pieces.n; ++j) {
                void* p = item(pieces, j);
                uint64_t k = 0;
                void* owner = nullptr;
                uint8_t dead = 1;
                if (!p || !mem_read((const uint8_t*)p + kPiece_Id, &k, sizeof(k)) || k != op.id ||
                    !mem_read((const uint8_t*)p + kComp_Owner, &owner, sizeof(owner)) || !owner ||
                    !mem_read((const uint8_t*)owner + 0x18, &dead, 1) || dead)
                    continue;
                if (safe_destroy(p)) ++gone;
                else log_line("HOUSE", "!! destroying furniture %llu's piece faulted", id);
            }
        if (gone > 1) log_line("HOUSE", "furniture %llu had %u pieces here -- all taken down", id, gone);
    }
    if (op.op == 3) {
        if (!safe_rec_drop(store, op.id)) log_line("HOUSE", "!! dropping furniture %llu faulted", id);
        else log_line("HOUSE", "<- furniture %llu gone, as the other player did", id);
        g_known.erase(op.id);
        return;
    }
    const std::string name = recs[op.id].name;
    if (!safe_sassign((uint8_t*)rec + 0x30, op.text.c_str())) {
        log_line("HOUSE", "!! furniture %llu's room could not be set", id);
        return;
    }
    mem_write((uint8_t*)rec + 0x50, &op.x, 4);
    mem_write((uint8_t*)rec + 0x54, &op.y, 4);
    uint8_t dying = 1;
    bool made = true;
    if (!op.text.empty())
        made = mem_read((const uint8_t*)scene + kScene_Dying, &dying, 1) && !dying && safe_new_piece(scene, op.id);
    g_known[op.id] = Placed{ name, op.text, op.x, op.y };
    log_line("HOUSE", made ? "<- furniture %llu '%s' %s %s (%d,%d), as the other player did"
                           : "!! furniture %llu '%s' %s %s (%d,%d) -- its piece could not be made here",
             id, name.c_str(), op.text.empty() ? "put away" : "put in", op.text.c_str(), op.x, op.y);
}
} // namespace

bool house_night_settled() {
    if (!g_night_at || !o_inter_update) return true;
    const uint64_t now = GetTickCount64();
    static uint64_t mtime = 0, since = 0;
    wchar_t path[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    uint64_t m = 0;
    if (savefile_live_path(path, MAX_PATH) && GetFileAttributesExW(path, GetFileExInfoStandard, &fad))
        m = ((uint64_t)fad.ftLastWriteTime.dwHighDateTime << 32) | fad.ftLastWriteTime.dwLowDateTime;
    if (m != mtime) { mtime = m; since = now; }
    const uint64_t inter = g_inter_at;
    const bool screens_done = inter > g_night_at ? now - inter > 2000 : now - g_night_at > 8000;
    const bool written      = now - since > 1500;
    if (now - g_night_at > 180000 || (screens_done && written)) {
        log_line("HOUSE", "this game's night is over (%llu ms after the sign)",
                 (unsigned long long)(now - g_night_at));
        g_night_at = 0;
        return true;
    }
    return false;
}

bool house_local_night() {
    return g_local_at && GetTickCount64() - g_local_at < 10 * 60 * 1000;
}

int house_morning(const uint8_t* data, uint32_t size) {
    if (!house_local_night()) return 2;
    if (!house_night_settled()) return 0;
    const uint64_t waited = GetTickCount64() - g_local_at;
    std::vector<Row> host, mine;
    wchar_t path[MAX_PATH];
    if (!parse_house(data, size, host) || !savefile_live_path(path, MAX_PATH) || !read_house(path, mine, nullptr)) {
        g_local_at = 0;
        log_line("HOUSE", "!! a morning save did not read -- the host's is loaded");
        return 2;
    }
    std::map<std::string, const Row*> here;
    for (const Row& r : mine) here[r.id] = &r;
    std::string host_day, my_day;
    for (const Row& r : host) if (r.id == "properties current_day") host_day = r.shown;
    if (here.count("properties current_day")) my_day = here["properties current_day"]->shown;
    if (my_day != host_day) {
        if (waited < kMorningWaitMs) return 0;
        g_local_at = 0;
        log_line("HOUSE", "!! this game's save is still on day %s, the host's on %s -- the host's is loaded",
                 my_day.c_str(), host_day.c_str());
        return 2;
    }
    uint32_t differ = 0, shown = 0;
    std::map<std::string, bool> seen;
    for (const Row& r : host) {
        seen[r.id] = true;
        if (ignored(r.id)) continue;
        auto it = here.find(r.id);
        const bool same = it != here.end() && it->second->hash == r.hash;
        if (same) continue;
        if (const char* why = noted(r.id)) {
            log_line("HOUSE", "  (not counted: %s) %s differs in the morning", why, r.id.c_str());
            continue;
        }
        ++differ;
        if (shown++ < 40)
            log_line("HOUSE", "  morning %s: host %s, here %s", label(r.id).c_str(), r.shown.c_str(),
                     it == here.end() ? "(none)" : it->second->shown.c_str());
    }
    for (const Row& r : mine) {
        if (seen.count(r.id) || ignored(r.id) || noted(r.id)) continue;
        ++differ;
        if (shown++ < 40) log_line("HOUSE", "  morning %s: only here, %s", label(r.id).c_str(), r.shown.c_str());
    }
    g_local_at = 0;
    for (const Row& r : host) {
        if (r.id != "properties current_house_weather") continue;
        void* w = weather_str();
        char now[64] = {};
        if (w && mem_read_std_string(w, now, sizeof(now)) && r.shown != now) {
            const bool ok = safe_sassign(w, r.shown.c_str());
            log_line("HOUSE", ok ? "the morning's weather is the host's: '%s' (this game drew '%s')"
                                 : "!! the host's weather '%s' could not be set (this game drew '%s')",
                     r.shown.c_str(), now);
        }
    }
    if (differ) {
        log_line("HOUSE", "!! the night came out different here (%u row(s)) -- the host's morning is loaded", differ);
        send("house_verdict", "the friend's night came out different (" + std::to_string(differ) +
                              " row(s)) -- it loaded the host's morning");
        return 2;
    }
    log_line("HOUSE", "the night came out the same here (%u rows, day %s) -- no reload", (unsigned)host.size(),
             host_day.c_str());
    send("house_verdict", "the friend's night came out the same (day " + host_day + ") -- no reload");
    return 1;
}

namespace {
bool night_shared() {
    return g_scenes_on && g_night_at && o_inter_ok && net_active() && net_peer_count() >= 2;
}
} // namespace

void house_on_scene(void* scene) {
    if (!g_scenes_on || !g_night_at) return;
    g_cur_scene   = scene;
    g_cur_scene_n = g_scene_n + 1;
    uint64_t* s = rng_global_stream();
    if (!s) return;
    uint64_t* v = s + 4;   // TLS+0x198
    const uint64_t base = g_seed[0] ^ (g_seed[2] << 1) ^ (0x9E3779B97F4A7C15ull * (uint64_t)(++g_scene_n));
    for (int i = 0; i < 4; ++i) v[i] = mix64(base + 0x632BE59BD9B4E019ull * (uint64_t)(i + 1));
    log_line("HOUSE", "night scene %u starts from the shared stream", g_scene_n);
}

void house_night_starts(const uint64_t stream[4]) {
    if (!net_active() || net_role() != NetRole::Host || net_peer_count() < 2) return;
    house_on_end_day();
    if (!o_resolve) return;
    g_night_at = GetTickCount64();
    memcpy(g_seed, stream, sizeof(g_seed));
    g_seed_at = GetTickCount64();
    ChoiceMsg m{};
    m.kind      = kChoiceNightStart;
    m.node_seed = stream[0];
    m.cat       = stream[1];
    snprintf(m.name, sizeof(m.name), "%016llx%016llx", (unsigned long long)stream[2], (unsigned long long)stream[3]);
    net_send_choice(m);
    log_line("HOUSE", "-> the night begins, from the stream %016llx...", (unsigned long long)stream[0]);
}

void house_on_end_day() {
    if (!net_active() || net_role() != NetRole::Host || net_peer_count() < 2) return;
    // The House's storage, backpack and trash go first: an NPC office's
    // reward was in the host's storage only (2026-11-15).
    invsync_forget();
    invsync_publish("the day ends");
    wchar_t path[MAX_PATH];
    std::vector<Row> rows;
    uint64_t age = 0;
    if (!savefile_live_path(path, MAX_PATH) || !read_house(path, rows, &age)) {
        log_line("HOUSE", "!! could not read the host's save -- the House is not checked");
        return;
    }
    live_rows(rows);
    std::string text;
    char hex[24];
    for (const Row& r : rows) {
        snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)r.hash);
        std::string shown = r.shown.size() > 80 ? r.shown.substr(0, 80) : r.shown;
        for (char& c : shown) if (c == '\n' || c == '\t') c = ' ';
        text += r.id + "\t" + hex + "\t" + shown + "\n";
    }
    send("house", text);
    log_line("HOUSE", "-> the day ends: the host's House, %u rows (its save written %llu ms before), to compare",
             (unsigned)rows.size(), (unsigned long long)age);
}

bool house_on_message(const LogFileMsg& m) {
    if (strcmp(m.reason, "house") == 0) {
        if (net_role() != NetRole::Host && m.data && m.size)
            compare(std::string((const char*)m.data, m.size));
        return true;
    }
    if (strcmp(m.reason, "house_verdict") == 0) {
        if (net_role() == NetRole::Host && m.data && m.size)
            log_line("HOUSE", "<- %.*s", (int)m.size, (const char*)m.data);
        return true;
    }
    return false;
}

} // namespace mgmp
