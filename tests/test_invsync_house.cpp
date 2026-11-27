// Compile the actual inventory receiver with fake native bucket functions.
// This lets us check the lifetime boundary without starting Mewgenics.
#include "../src/session/mgmp_invsync.cpp"

namespace mgmp {
namespace {
Config test_config;
bool replace_ready = false;
unsigned reads = 0, rebuilds = 0;
bool rebuild_order_ok = true;
uint8_t director[1600]{}, inventory[512]{};
void* director_ptr = director;

void __fastcall destroy_key(void* p) {
    auto* key = (GameStr*)p;
    if (key->cap > 15) free(key->ptr);
}
void* __fastcall allocate(size_t n) { return malloc(n); }
void __fastcall write_bucket(void*, void*, void* key, void* bucket) {
    g.cap_buf = (uint8_t*)malloc(4);
    memcpy(g.cap_buf, bucket, 4);
    g.cap_len = 4; g.cap_hit = true;
    destroy_key(key);
}
void __fastcall read_bucket(void*, void*, void* key, void* bucket) {
    memcpy(bucket, g.sup_buf, g.sup_len);
    ++reads; g.sup_hit = true;
    destroy_key(key);
}
void* __fastcall unused_box(void*, void*, void*, void*) { return nullptr; }
}
const Config& config() { return test_config; }
bool hooks_is_live(int) { return true; }
uintptr_t addr_of_call(Call) { return 0; }
uintptr_t addr_of_data(DataSym) { return 0; }
bool net_active() { return true; }
bool net_send_inventory(const InventoryMsg&) { return true; }
bool leave_on_house() { return true; }
bool house_inventory_can_replace() { return replace_ready; }
void house_inventory_replaced() {
    rebuild_order_ok = rebuild_order_ok && reads == 3;
    ++rebuilds;
}
void log_line(const char*, const char*, ...) {}
void log_line_lvl(LogLevel, const char*, const char*, ...) {}

int exercise() {
    strcpy_s(test_config.net_role, "client");
    test_config.net_follow = false; // House loads must defer even without follow.
    void* inv = inventory;
    memcpy(director + kDir_Inventory, &inv, sizeof(inv));
    g.resolved = true; g.director_slot = (const void**)&director_ptr;
    g.bucket_write = write_bucket; g.bucket_read = read_bucket;
    g.str_alloc = allocate; g.str_dtor = destroy_key;
    o_box_update = unused_box;

    InventoryMsg snapshot{};
    uint32_t old_value = 17, latest_value = 29;
    for (unsigned i = 0; i < kInvBuckets; ++i) {
        snapshot.data[i] = (uint8_t*)&old_value; snapshot.size[i] = 4;
    }
    snapshot.hash = 123;
    invsync_on_message(snapshot);
    if (reads || !invsync_has_pending() || invsync_apply_pending("menu open")) return 1;
    for (unsigned i = 0; i < kInvBuckets; ++i) snapshot.data[i] = (uint8_t*)&latest_value;
    snapshot.hash = 456;
    invsync_on_message(snapshot);
    replace_ready = true;
    if (!invsync_apply_pending("menu closed") || reads != 3 || rebuilds != 1 ||
        !rebuild_order_ok || invsync_has_pending()) return 2;
    for (const auto& bucket : kBuckets) {
        uint32_t actual = 0; memcpy(&actual, inventory + bucket.off, 4);
        if (actual != latest_value) return 3;
    }
    // Identical echoes should keep existing items/buttons alive.
    snapshot.hash = invsync_local_hash();
    invsync_on_message(snapshot);
    if (!invsync_apply_pending("echo") || reads != 3 || rebuilds != 1) return 4;
    // An open screen with no item-box updates still blocks node clicks and
    // inventory replacement. Reproduce an empty backpack / inactive boxes.
    g_box_at = 0;
    if (invsync_screen_open()) return 5;
    invsync_on_screen_tick();
    if (!invsync_screen_open()) return 6;
    g_screen_at = GetTickCount64() - kBoxGoneMs - 1;
    if (invsync_screen_open()) return 7;
    invsync_shutdown();
    return 0;
}
}
int main() {
    const int result = mgmp::exercise();
    if (result) fprintf(stderr, "House inventory lifetime check failed at step %d\n", result);
    return result;
}
