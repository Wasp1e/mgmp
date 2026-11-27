#include "mgmp_house_shop.h"
#include "mgmp_house_shop_rules.h"
#include "mgmp_addresses.h"
#include "mgmp_resolve.h"
#include "mgmp_bytestream.h"
#include "mgmp_cutscene.h"
#include "mgmp_invsync.h"
#include "mgmp_house.h"
#include "mgmp_leave.h"
#include "mgmp_follow.h"
#include "mgmp_mem.h"
#include "mgmp_net.h"
#include "mgmp_proto.h"
#include "mgmp_log.h"
#include <windows.h>
#include <cstring>
#include <cstdio>
#include <vector>
#include <mutex>

namespace mgmp {
namespace {
constexpr uint8_t kShopRequest = kChoiceHouseShopBuy;
constexpr uint8_t kShopStock = kChoiceHouseShopStock;
constexpr size_t kItemSize = 0xC0;
constexpr uint32_t kMaxStock = 256, kMaxProgress = 1024 * 1024;
struct HouseShopState { int32_t coins, food, storage_upgrades; };
static_assert(sizeof(HouseShopState) == 12);
constexpr size_t kShopProgressOffset = sizeof(uint64_t) + sizeof(HouseShopState);
uintptr_t base = 0;
bool verified = false;
HouseShopBuy original_buy = nullptr;
using ShopRefresh = void (__fastcall*)(void*);
ShopRefresh refresh_shop = nullptr;
using StreamCall = void (__fastcall*)(void*, void*);
using StreamDtor = void (__fastcall*)(void*);
using StreamCtor = void* (__fastcall*)(void*);
std::mutex pending_lock;
std::vector<uint8_t> pending;
struct QueuedChoice { ChoiceMsg msg; uint64_t at; };
std::vector<QueuedChoice> requests, stock_updates;
uint64_t sent_hash = 0, sent_at = 0, sent_day = 0;
void* sent_director = nullptr;
uint64_t client_shop_at = 0;

bool context(void*& director, void*& progress, uint64_t& day) {
    auto slot = addr_of_data(D_MewDirectorPtr);
    return slot && mem_read((void*)slot, &director, sizeof(director)) && director &&
        mem_read((uint8_t*)director + 0x5A8, &progress, sizeof(progress)) && progress &&
        mem_read((uint8_t*)director + 0x580, &day, sizeof(day));
}
bool linked() { return verified && net_active() && net_peer_count() >= 2; }
uint64_t hash(const void* data, size_t size) {
    uint64_t h = 0xCBF29CE484222325ull;
    auto p = (const uint8_t*)data;
    for (size_t i = 0; i < size; ++i) { h ^= p[i]; h *= 0x100000001B3ull; }
    return h;
}
bool stream_init(uint8_t* stream) {
    __try {
        memset(stream, 0, kByteStreamSize);
        ((StreamCtor)addr_of_call(C_OfstreamCtor))(stream + kBS_Ofstream);
        *(uint32_t*)(stream + kBS_SwapElem) = 8;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool stream_call(void* progress, uint8_t* stream) {
    __try {
        // GlobalProgressionData's npc_progress serializer, called by its
        // save/load paths at 0x1D25BE / 0x1D2Cxx. Includes shop stock,
        // unlocks, purchased collars, upgrades and quest progression.
        ((StreamCall)(base + 0x1D3B90))(progress, stream);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void stream_destroy(uint8_t* stream) {
    __try { ((StreamDtor)addr_of_call(C_ByteStreamDtor))(stream); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}
bool serialize(void* progress, std::vector<uint8_t>& bytes) {
    alignas(16) uint8_t stream[kByteStreamSize];
    if (!stream_init(stream)) return false;
    *(uint32_t*)(stream + kBS_Mode) = 1;
    bool ok = stream_call(progress, stream);
    const uint32_t n = *(uint32_t*)(stream + kBS_WriteLen);
    const void* p = *(void**)(stream + kBS_WriteBuf);
    ok = ok && p && n >= 4 && n <= kMaxProgress;
    if (ok) { bytes.resize(n); ok = mem_read(p, bytes.data(), n); }
    stream_destroy(stream);
    return ok;
}
bool deserialize(void* progress, std::vector<uint8_t>& bytes) {
    alignas(16) uint8_t stream[kByteStreamSize];
    if (!stream_init(stream)) return false;
    // Borrow our buffer; the game's destructor must not free it.
    *(void**)(stream + kBS_ReadBuf) = bytes.data();
    *(uint32_t*)(stream + kBS_ReadLen) = (uint32_t)bytes.size();
    const bool ok = stream_call(progress, stream) &&
        *(uint32_t*)(stream + kBS_ReadPos) == bytes.size();
    stream_destroy(stream);
    return ok;
}
bool shop_items(void* shop, uint8_t*& begin, uint32_t& count) {
    uint8_t house = 0;
    uintptr_t first = 0, end = 0;
    if (!shop || !mem_read((uint8_t*)shop + 0x7B, &house, 1) ||
        !mem_read((uint8_t*)shop + 0x60, &first, sizeof(first)) ||
        !mem_read((uint8_t*)shop + 0x68, &end, sizeof(end)) || !first || end < first ||
        (end - first) % kItemSize || (end - first) / kItemSize > kMaxStock) return false;
    begin = (uint8_t*)first; count = (uint32_t)((end - first) / kItemSize);
    return true;
}
bool describe(void* item, ChoiceMsg& m) {
    uint32_t identity[3], quantity = 0;
    if (!mem_read(item, identity, sizeof(identity)) ||
        !mem_read((uint8_t*)item + 0xC, &quantity, 4)) return false;
    m.cat = hash(identity, sizeof(identity)); // type, GON id and price; no pointers
    m.count = quantity;
    return true;
}
uint64_t catalog_hash(uint8_t* begin, uint32_t count) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint32_t i = 0; i < count; ++i) {
        ChoiceMsg item{};
        if (!describe(begin + i * kItemSize, item)) return 0;
        h ^= item.cat; h *= 0x100000001B3ull;
    }
    return h;
}
bool safe_buy(void* shop, void* item) {
    __try { original_buy(shop, item); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool safe_refresh(void* shop) {
    if (!refresh_shop) return false;
    __try { refresh_shop(shop); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}

void house_shop_set_base(uintptr_t value) {
    base = value;
    // The native purchase callback calls this after decrementing stock
    // (0x79A967). It reconciles shelf movies/buttons with ShopItem quantities;
    // Shop::update alone never does that reconciliation.
    const uint8_t refresh_head[] = {0x48,0x89,0x5C,0x24,0x18,0x48,0x89,0x74,0x24,0x20};
    uint8_t refresh_actual[sizeof(refresh_head)]{};
    refresh_shop = mem_read((void*)(base + 0x798610), refresh_actual, sizeof(refresh_actual)) &&
        !memcmp(refresh_actual, refresh_head, sizeof(refresh_head))
        ? (ShopRefresh)(base + 0x798610) : nullptr;
    if (!refresh_shop) log_line("HOUSESHOP", "!! native shelf refresh did not verify");
    const uint8_t head[] = {0x48,0x89,0x5C,0x24,0x08,0x55,0x56,0x57,0x41,0x54};
    uint8_t actual[sizeof(head)]{};
    verified = addr_of_call(C_OfstreamCtor) && addr_of_call(C_ByteStreamDtor) &&
        mem_read((void*)(base + 0x1D3B90), actual, sizeof(actual)) &&
        memcmp(actual, head, sizeof(head)) == 0;
    if (!verified) log_line("HOUSESHOP", "!! native shop progression serializer did not verify");
}

void house_shop_sync(bool force) {
    if (!linked()) { sent_hash = 0; return; }
    if (net_role() != NetRole::Host || !leave_on_house()) return;
    const uint64_t now = GetTickCount64();
    if (!force && now - sent_at < 500) return;
    sent_at = now;
    void* director = nullptr; void* progress = nullptr; uint64_t day = 0;
    if (!context(director, progress, day)) return;
    if (director != sent_director || day != sent_day) {
        sent_hash = 0; sent_director = director; sent_day = day;
    }
    std::vector<uint8_t> bytes;
    if (!serialize(progress, bytes)) return;
    void* store = nullptr;
    HouseShopState house_state{};
    if (!mem_read((void*)(base + 0x13DA9A0), &store, sizeof(store)) || !store ||
        !mem_read((uint8_t*)store + 0xB4, &house_state.coins, 4) ||
        !mem_read((uint8_t*)store + 0xB0, &house_state.food, 4) ||
        !mem_read((uint8_t*)store + kHouseStoreStorageUpgrade, &house_state.storage_upgrades, 4)) return;
    const uint64_t h = hash(bytes.data(), bytes.size()) ^ hash(&house_state, sizeof(house_state));
    if (!force && h == sent_hash) return;
    std::vector<uint8_t> packet(kShopProgressOffset + bytes.size());
    memcpy(packet.data(), &day, sizeof(day));
    memcpy(packet.data() + sizeof(day), &house_state, sizeof(house_state));
    memcpy(packet.data() + kShopProgressOffset, bytes.data(), bytes.size());
    LogFileMsg m{};
    strcpy_s(m.reason, "house_shop");
    m.data = packet.data(); m.size = (uint32_t)packet.size();
    if (net_send_night(m)) {
        sent_hash = h;
        log_line("HOUSESHOP", "-> day %llu progression: %u bytes hash=%016llx, House storage upgrades=%d",
                 (unsigned long long)day, (unsigned)bytes.size(), (unsigned long long)h, house_state.storage_upgrades);
        // Send capacity before the matching inventory even if the client
        // processes these messages on separate frames.
        invsync_publish("House shop/progression change");
    }
}

void house_shop_apply() {
    if (!linked() || net_role() == NetRole::Host || !leave_on_house() || cutscene_active() ||
        invsync_screen_open() || !house_inventory_can_replace()) return;
    void* director = nullptr; void* progress = nullptr; uint64_t day = 0, wanted_day = 0;
    if (!context(director, progress, day)) return;
    void* store = nullptr;
    if (!mem_read((void*)(base + 0x13DA9A0), &store, sizeof(store)) || !store) return;
    std::vector<uint8_t> packet;
    { std::lock_guard<std::mutex> guard(pending_lock); packet.swap(pending); }
    if (packet.empty()) {
        if (!leave_loading(nullptr) && invsync_has_pending()) invsync_apply_pending("House shop closed");
        return;
    }
    memcpy(&wanted_day, packet.data(), sizeof(wanted_day));
    if (day != wanted_day) {
        if (day < wanted_day) {
            std::lock_guard<std::mutex> guard(pending_lock);
            if (pending.empty()) pending.swap(packet);
        }
        return;
    }
    HouseShopState house_state{};
    memcpy(&house_state, packet.data() + sizeof(wanted_day), sizeof(house_state));
    std::vector<uint8_t> bytes(packet.begin() + kShopProgressOffset, packet.end()), current;
    const bool same = serialize(progress, current) && current == bytes;
    // The House's upgrade sets the drawer width. NPC progression adds bonus
    // slots separately. Copying the latter over the former made a 36-slot
    // drawer overflow into trash. Set capacity before applying its contents.
    const bool ok = (same || deserialize(progress, bytes)) &&
        mem_write((uint8_t*)store + kHouseStoreStorageUpgrade, &house_state.storage_upgrades, 4) &&
        mem_write((uint8_t*)store + 0xB4, &house_state.coins, 4) &&
        mem_write((uint8_t*)store + 0xB0, &house_state.food, 4);
    log_line("HOUSESHOP", ok ? "<- applied day %llu progression and House storage upgrades=%d"
                             : "!! applying day %llu progression or House storage upgrades=%d failed",
             (unsigned long long)day, house_state.storage_upgrades);
    if (ok && invsync_has_pending()) invsync_apply_pending("after House storage capacity sync");
}

bool house_shop_on_message(const LogFileMsg& m) {
    if (strcmp(m.reason, "house_shop")) return false;
    if (!linked() || net_role() == NetRole::Host || !m.data ||
        m.size < kShopProgressOffset + sizeof(uint32_t) || m.size > kMaxProgress + kShopProgressOffset)
        return true;
    uint32_t version = 0; memcpy(&version, m.data + kShopProgressOffset, sizeof(version));
    if (version != 38) return true; // this build's native serializer version
    std::lock_guard<std::mutex> guard(pending_lock);
    pending.assign(m.data, m.data + m.size);
    return true;
}

bool house_shop_buy(void* shop, void* item, HouseShopBuy buy) {
    original_buy = buy;
    if (!net_active() || net_role() == NetRole::Host) return false;
    log_line("HOUSESHOP", "purchase refused: only the host buys");
    return true;
}


bool house_shop_on_choice(const ChoiceMsg& m, uint8_t from) {
    if (m.kind != kShopRequest && m.kind != kShopStock) return false;
    if (m.kind == kShopRequest) return true;
    if (!linked() || from == net_self() || from >= kMaxPeers || m.index >= kMaxStock ||
        (m.kind == kShopRequest ? net_role() != NetRole::Host :
         net_role() == NetRole::Host || from != 0)) return true;
    std::lock_guard<std::mutex> guard(pending_lock);
    auto& queue = m.kind == kShopRequest ? requests : stock_updates;
    if (queue.size() < kMaxStock) queue.push_back({m, GetTickCount64()});
    return true;
}

void house_shop_update(void* shop) {
    if (!linked()) return;
    uint8_t* begin = nullptr; uint32_t count = 0;
    void* director = nullptr; void* progress = nullptr; uint64_t day = 0;
    if (!shop_items(shop, begin, count) || !context(director, progress, day)) return;
    const bool host = net_role() == NetRole::Host;
    uint8_t house = 0;
    mem_read((uint8_t*)shop + 0x7B, &house, sizeof(house));
    if (!host && house) client_shop_at = GetTickCount64();
    const uint64_t catalog = catalog_hash(begin, count);
    std::vector<QueuedChoice> work;
    { std::lock_guard<std::mutex> guard(pending_lock);
      work.swap(host ? requests : stock_updates); }
    bool stock_changed = false;
    for (const auto& queued : work) {
        const auto& m = queued.msg;
        unsigned long long expected_catalog = 0;
        const uint64_t scope = house ? day : follow_here_seed();
        if (GetTickCount64() - queued.at > 2000 || m.node_seed != scope ||
            m.aux != (uint32_t)house || m.index >= count ||
            sscanf_s(m.name, "%16llx", &expected_catalog) != 1 || expected_catalog != catalog) continue;
        void* item = begin + m.index * kItemSize;
        ChoiceMsg local{};
        if (!describe(item, local) || local.cat != m.cat) {
            log_line("HOUSESHOP", "!! shop slot %u differs; update/request refused", m.index);
            continue;
        }
        if (!host) {
            if (local.count != m.count && refresh_shop &&
                mem_write((uint8_t*)item + 0xC, &m.count, 4)) stock_changed = true;
            continue;
        }
        if (!original_buy) original_buy = (HouseShopBuy)addr_of(T_ShopBuy);
        // A mirrored host click carries the OLD count: reject that echo,
        // simultaneous stale requests, and sold-out items before any grant.
        if (!house_shop_request_current(GetTickCount64() - queued.at, day, m.node_seed,
                                       catalog, expected_catalog, (int32_t)local.count,
                                       (int32_t)m.count) || !original_buy) {
            log_line("HOUSESHOP", "purchase request slot=%u declined: stock changed", m.index);
            continue;
        }
        if (safe_buy(shop, item)) log_line("HOUSESHOP", "<- host accepted purchase request slot=%u", m.index);
    }
    if (stock_changed) {
        const bool ok = safe_refresh(shop);
        log_line("HOUSESHOP", ok ? "<- refreshed shop shelves after host stock change"
                                 : "!! refreshing shop shelves after host stock change failed");
    }
    if (host) {
        // Only scalar stock counts are replaced while the client's Shop UI
        // holds ShopItem pointers. Full progression is applied after it closes.
        static uint64_t last_stock_at = 0;
        static uint64_t last_stock_hash = 0;
        uint64_t stock_hash = catalog;
        for (uint32_t i = 0; i < count; ++i) {
            ChoiceMsg value{};
            if (describe(begin + i * kItemSize, value)) {
                stock_hash ^= value.count;
                stock_hash *= 0x100000001B3ull;
            }
        }
        const uint64_t now = GetTickCount64();
        if (stock_hash != last_stock_hash || now - last_stock_at >= 500) {
            last_stock_hash = stock_hash;
            last_stock_at = now;
            void* store = nullptr;
            ChoiceMsg purse{};
            if (house && mem_read((void*)(base + 0x13DA9A0), &store, sizeof(store)) && store &&
                mem_read((uint8_t*)store + 0xB4, &purse.index, 4) &&
                mem_read((uint8_t*)store + 0xB0, &purse.count, 4)) {
                purse.kind = kChoiceHousePurse;
                net_send_choice(purse);
            }
            for (uint32_t i = 0; i < count; ++i) {
                ChoiceMsg m{};
                if (!describe(begin + i * kItemSize, m)) continue;
                m.kind = kShopStock; m.index = i;
                m.node_seed = house ? day : follow_here_seed();
                m.aux = house;
                snprintf(m.name, sizeof(m.name), "%016llx", (unsigned long long)catalog);
                net_send_choice(m);
            }
        }
        if (house) house_shop_sync();
    }
}

uint64_t house_shop_hash() {
    void* director = nullptr; void* progress = nullptr; uint64_t day = 0;
    std::vector<uint8_t> bytes;
    return verified && context(director, progress, day) && serialize(progress, bytes)
        ? hash(bytes.data(), bytes.size()) : 0;
}
bool house_shop_client_active() {
    return linked() && net_role() != NetRole::Host && client_shop_at &&
           GetTickCount64() - client_shop_at < 300;
}
}
