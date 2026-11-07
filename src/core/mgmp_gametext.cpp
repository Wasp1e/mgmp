#include "mgmp_gametext.h"
#include "mgmp_lang.h"
#include "mgmp_gpak.h"
#include "mgmp_log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mgmp {
namespace {

const char* const kItemFiles[] = {
    "data/items/armor_sets.gon",     "data/items/beanies_quest_items.gon",
    "data/items/consumables.gon",    "data/items/cursed_items.gon",
    "data/items/enemy_items.gon",    "data/items/face_items.gon",
    "data/items/head_items.gon",     "data/items/legacy_quest_items.gon",
    "data/items/legendary_items.gon","data/items/modifiers.gon",
    "data/items/neck_items.gon",     "data/items/parasites.gon",
    "data/items/special_class_items.gon", "data/items/trinkets.gon",
    "data/items/weapons.gon",
};

// The bonuses an item's gon entry gives as plain numbers, in this order.
struct Bonus { const char* key; const char* label; const char* en; };
const Bonus kBonuses[] = {
    { "str", u8"\u0421\u0438\u043b\u0430", "Strength" }, { "dex", u8"\u041b\u043e\u0432\u043a\u043e\u0441\u0442\u044c", "Dexterity" }, { "con", u8"\u0422\u0435\u043b\u043e\u0441\u043b\u043e\u0436\u0435\u043d\u0438\u0435", "Constitution" }, { "int", u8"\u0418\u043d\u0442\u0435\u043b\u043b\u0435\u043a\u0442", "Intelligence" },
    { "spd", u8"\u0421\u043a\u043e\u0440\u043e\u0441\u0442\u044c", "Speed" }, { "cha", u8"\u0425\u0430\u0440\u0438\u0437\u043c\u0430", "Charisma" }, { "lck", u8"\u0423\u0434\u0430\u0447\u0430", "Luck" }, { "shield", u8"\u0429\u0438\u0442", "Shield" },
};
constexpr size_t kNumBonuses = sizeof(kBonuses) / sizeof(kBonuses[0]);

struct Item {
    std::string name_key, desc_key, variant_of, durability, kind, frame;
    int  bonus[kNumBonuses] = {};
};

struct State {
    bool loaded = false;
    std::unordered_map<std::string, Item>        items;
    std::unordered_map<std::string, std::string> text;   // key -> Russian
};

State g;

// --- gon ---------------------------------------------------------------------
//
// Enough of it for item entries: `Key { field value ... }`, values being words,
// numbers, "strings", [lists] or { blocks }, // comments to the end of the line.
struct Lexer {
    const char* p;
    const char* end;
    bool next(std::string& tok) {
        for (;;) {
            while (p < end && (unsigned char)*p <= ' ') ++p;
            if (p + 1 < end && p[0] == '/' && p[1] == '/') {
                while (p < end && *p != '\n') ++p;
                continue;
            }
            break;
        }
        if (p >= end) return false;
        if (*p == '{' || *p == '}' || *p == '[' || *p == ']' || *p == ',') {
            tok.assign(p, 1); ++p; return true;
        }
        if (*p == '"') {
            const char* s = ++p;
            while (p < end && *p != '"') ++p;
            tok.assign(s, p - s);
            if (p < end) ++p;
            tok.insert(tok.begin(), '\x01');   // marks a string
            return true;
        }
        const char* s = p;
        while (p < end && (unsigned char)*p > ' ' && !strchr("{}[],\"", *p)) ++p;
        tok.assign(s, p - s);
        return true;
    }
};

std::string unquote(const std::string& t) { return !t.empty() && t[0] == '\x01' ? t.substr(1) : t; }

// Reads one value after a field name; a block or a list is skipped whole, but the
// first word of a list is kept (durability [7, 12] -> "7-12").
std::string read_value(Lexer& lx) {
    std::string t;
    if (!lx.next(t)) return "";
    if (t == "{" || t == "[") {
        const std::string open = t, close = t == "{" ? "}" : "]";
        std::string first, last;
        int depth = 1;
        while (depth && lx.next(t)) {
            if (t == "{" || t == "[") ++depth;
            else if (t == "}" || t == "]") --depth;
            else if (depth == 1 && t != "," && open == "[") {
                if (first.empty()) first = unquote(t);
                last = unquote(t);
            }
        }
        return first.empty() ? "" : first == last ? first : first + "-" + last;
    }
    return unquote(t);
}

void parse_gon(const char* data, size_t size) {
    Lexer lx{ data, data + size };
    std::string key, t;
    while (lx.next(key)) {
        if (!lx.next(t) || t != "{") continue;
        Item it;
        std::string field;
        while (lx.next(field) && field != "}") {
            if (field == "{" ) { read_value(lx); continue; }
            const std::string v = read_value(lx);
            if (field == "name") it.name_key = v;
            else if (field == "desc") it.desc_key = v;
            else if (field == "variant_of") it.variant_of = v;
            else if (field == "durability") it.durability = v;
            else if (field == "kind") it.kind = v;
            else if (field == "frame") it.frame = v;
            else
                for (size_t b = 0; b < kNumBonuses; ++b)
                    if (field == kBonuses[b].key) it.bonus[b] = atoi(v.c_str());
        }
        g.items[unquote(key)] = it;
    }
}

// --- combined.csv ------------------------------------------------------------
//
// KEY,en,notes,sp,fr,de,it,pt-br,ru,... with quoted fields that may hold commas,
// doubled quotes and newlines. Only the keys an item names are kept.
void parse_csv(const char* p, const char* end, const std::unordered_set<std::string>& want) {
    std::vector<std::string> row;
    std::string field;
    int ru = -1;
    bool header = true;
    auto end_row = [&]() {
        if (header) {
            // The game's own language column, English if it has none.
            for (size_t i = 0; i < row.size(); ++i) if (row[i] == lang_code()) ru = (int)i;
            if (ru < 0) for (size_t i = 0; i < row.size(); ++i) if (row[i] == "en") ru = (int)i;
            header = false;
        } else if (ru >= 0 && (int)row.size() > ru && want.count(row[0])) {
            g.text[row[0]] = row[ru];
        }
        row.clear();
    };
    if (end - p >= 3 && (uint8_t)p[0] == 0xEF && (uint8_t)p[1] == 0xBB && (uint8_t)p[2] == 0xBF) p += 3;
    while (p < end) {
        field.clear();
        if (*p == '"') {
            ++p;
            while (p < end) {
                if (*p == '"') {
                    if (p + 1 < end && p[1] == '"') { field += '"'; p += 2; continue; }
                    ++p; break;
                }
                field += *p++;
            }
        } else {
            while (p < end && *p != ',' && *p != '\n' && *p != '\r') field += *p++;
        }
        row.push_back(field);
        if (p < end && *p == ',') { ++p; continue; }
        while (p < end && (*p == '\r' || *p == '\n')) ++p;
        end_row();
    }
    if (!row.empty()) end_row();
}

void load() {
    if (g.loaded) return;
    g.loaded = true;
    for (const char* f : kItemFiles) {
        uint8_t* data = nullptr;
        uint32_t size = 0;
        if (!gpak_read(f, &data, &size)) continue;
        parse_gon((const char*)data, size);
        free(data);
    }
    std::unordered_set<std::string> want;
    for (const auto& kv : g.items) {
        if (!kv.second.name_key.empty()) want.insert(kv.second.name_key);
        if (!kv.second.desc_key.empty()) want.insert(kv.second.desc_key);
    }
    uint8_t* csv = nullptr;
    uint32_t size = 0;
    if (gpak_read("data/text/combined.csv", &csv, &size)) {
        parse_csv((const char*)csv, (const char*)csv + size, want);
        free(csv);
    }
    log_line("UI", "item texts: %zu items, %zu Russian lines", g.items.size(), g.text.size());
}

// [img:str], [s:.9], [/s] and the like are the game's markup, not words.
void strip_markup(const std::string& in, char* out, size_t cap) {
    size_t n = 0;
    for (size_t i = 0; i < in.size() && n + 1 < cap; ++i) {
        if (in[i] == '[') {
            const size_t close = in.find(']', i);
            if (close != std::string::npos) { i = close; continue; }
        }
        out[n++] = in[i];
    }
    out[n] = 0;
}

const Item* find(const std::string& key, int depth = 0) {
    auto it = g.items.find(key);
    return it == g.items.end() ? nullptr : &it->second;
}

} // namespace

bool gametext_item(const char* key, ItemText& out) {
    load();
    out = ItemText{};
    const Item* it = find(key);
    if (!it) { snprintf(out.name, sizeof(out.name), "%s", key); return false; }

    // A variant has its own numbers and its parent's words.
    const Item* words = it;
    for (int d = 0; d < 4 && words->name_key.empty() && !words->variant_of.empty(); ++d) {
        const Item* up = find(words->variant_of);
        if (!up) break;
        words = up;
    }
    auto text = [&](const std::string& k) -> const std::string* {
        auto t = g.text.find(k);
        return t == g.text.end() ? nullptr : &t->second;
    };
    // The picture too may be the parent's.
    const Item* pic = it;
    for (int d = 0; d < 4 && (pic->kind.empty() || pic->frame.empty()) && !pic->variant_of.empty(); ++d) {
        const Item* up = find(pic->variant_of);
        if (!up) break;
        pic = up;
    }
    if (!pic->kind.empty() && !pic->frame.empty())
        snprintf(out.icon, sizeof(out.icon), "%s_%s", pic->kind.c_str(), pic->frame.c_str());

    const std::string* name = text(words->name_key);
    if (name) strip_markup(*name, out.name, sizeof(out.name));
    else snprintf(out.name, sizeof(out.name), "%s", key);
    if (const std::string* desc = text(words->desc_key)) strip_markup(*desc, out.desc, sizeof(out.desc));

    size_t n = 0;
    for (size_t b = 0; b < kNumBonuses; ++b) {
        if (!it->bonus[b]) continue;
        n += snprintf(out.bonus + n, sizeof(out.bonus) - n, "%s%s %+d", n ? ", " : "",
                      tr(kBonuses[b].label, kBonuses[b].en), it->bonus[b]);
        if (n >= sizeof(out.bonus)) break;
    }
    if (!it->durability.empty() && n < sizeof(out.bonus))
        snprintf(out.bonus + n, sizeof(out.bonus) - n, "%s%s %s", n ? ", " : "", tr(u8"\u041f\u0440\u043e\u0447\u043d\u043e\u0441\u0442\u044c", "Durability"),
                 it->durability.c_str());
    return true;
}

} // namespace mgmp
