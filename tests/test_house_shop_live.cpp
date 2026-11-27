// Exercise the real stock receiver with a fake native shelf renderer.
#include "../src/session/mgmp_house_shop.cpp"

namespace mgmp {
namespace {
uint8_t test_director[0x600]{}, test_progress[8]{}, test_shop[0xA0]{};
uint8_t test_items[kItemSize * 2]{};
void* test_director_ptr = test_director;
unsigned refreshes = 0;
bool shelves_updated = false;
void __fastcall render_shelves(void*) {
    ++refreshes;
    uint32_t a = 1, b = 1;
    memcpy(&a, test_items + 0xC, 4);
    memcpy(&b, test_items + kItemSize + 0xC, 4);
    shelves_updated = a == 0 && b == 0;
}
}
uintptr_t addr_of(Target) { return 0; }
uintptr_t addr_of_call(Call) { return 0; }
uintptr_t addr_of_data(DataSym) { return (uintptr_t)&test_director_ptr; }
bool net_active() { return true; }
NetRole net_role() { return NetRole::Client; }
uint8_t net_self() { return 1; }
uint8_t net_peer_count() { return 2; }
bool net_send_choice(const ChoiceMsg&) { return true; }
bool net_send_night(const LogFileMsg&) { return true; }
bool leave_on_house() { return true; }
bool leave_loading(const char**) { return false; }
bool cutscene_active() { return false; }
bool invsync_screen_open() { return false; }
bool invsync_has_pending() { return false; }
bool invsync_apply_pending(const char*) { return true; }
void invsync_publish(const char*) {}
bool house_inventory_can_replace() { return true; }
uint64_t follow_here_seed() { return 99; }
void log_line(const char*, const char*, ...) {}

int exercise() {
    verified = true;
    refresh_shop = render_shelves;
    void* progress = test_progress;
    uint64_t day = 214;
    memcpy(test_director + 0x5A8, &progress, sizeof(progress));
    memcpy(test_director + 0x580, &day, sizeof(day));
    void* begin = test_items;
    void* end = test_items + sizeof(test_items);
    memcpy(test_shop + 0x60, &begin, sizeof(begin));
    memcpy(test_shop + 0x68, &end, sizeof(end));
    test_shop[0x7B] = 1;
    uint32_t one = 1;
    for (unsigned i = 0; i < 2; ++i) {
        memcpy(test_items + i * kItemSize, &i, 4);
        memcpy(test_items + i * kItemSize + 0xC, &one, 4);
    }
    ChoiceMsg messages[2]{};
    const auto catalog = catalog_hash(test_items, 2);
    for (unsigned i = 0; i < 2; ++i) {
        auto& m = messages[i];
        describe(test_items + i * kItemSize, m);
        m.kind = kShopStock; m.index = i; m.count = 0; m.aux = 1; m.node_seed = day;
        snprintf(m.name, sizeof(m.name), "%016llx", (unsigned long long)catalog);
        house_shop_on_choice(m, 0);
    }
    house_shop_update(test_shop);
    if (refreshes != 1 || !shelves_updated) return 1;
    // Periodic identical snapshots must not rebuild the shelf every 500 ms.
    for (const auto& m : messages) house_shop_on_choice(m, 0);
    house_shop_update(test_shop);
    if (refreshes != 1) return 2;
    // A different NPC/day/catalog must leave both stock and pictures alone.
    for (unsigned i = 0; i < 3; ++i) {
        auto m = messages[0]; m.count = 4;
        if (i == 0) ++m.node_seed;
        if (i == 1) strcpy_s(m.name, "0000000000000000");
        if (i == 2) ++m.cat;
        house_shop_on_choice(m, 0);
    }
    house_shop_update(test_shop);
    uint32_t actual = 1; memcpy(&actual, test_items + 0xC, 4);
    return refreshes == 1 && actual == 0 ? 0 : 3;
}
}
int main() { return mgmp::exercise(); }
