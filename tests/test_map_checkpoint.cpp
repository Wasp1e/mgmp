// Compile the actual map checkpoint path with a fake serializer and transport.
#include "../src/session/mgmp_follow.cpp"

namespace mgmp {
static unsigned saves = 0, starts = 0;
static uint32_t saved_node = 999;
static bool backpack_open = false;
static unsigned meows = 0;
static ChoiceMsg last_choice{};
void net_begin_battle_checkpoint() { ++starts; }
bool savefile_checkpoint_before_battle() {
    ++saves;
    return follow_map_save_safe() && follow_marker_node(saved_node);
}
void log_line(const char*, const char*, ...) {}
// Unrelated collaborators of the real follow module are inert in this test.
void catsync_publish(const char*) {}
void catsync_forget() {}
uint64_t catsync_party_hash() { return 0; }
bool catsync_apply_pending(const char*) { return true; }
uint64_t catsync_equipment_sig() { return 0; }
void catsync_send_equipment() {}
bool catsync_has_pending() { return false; }
void catsync_drop_pending() {}
void catsync_take_equipment(bool) {}
void catsync_mark_sent() {}
void choice_on_node_entered(uint64_t) {}
void choice_on_node_skipped(uint64_t) {}
void invsync_publish(const char*) {}
void invsync_forget() {}
bool invsync_apply_pending(const char*) { return true; }
bool invsync_screen_open() { return backpack_open; }
uint64_t invsync_local_hash() { return 0; }
void invsync_send_edit() {}
bool invsync_has_pending() { return false; }
void invsync_drop_pending() {}
int invsync_take_edit() { return 0; }
void nodehash_on_node(uint64_t, uint32_t) {}
void runhist_publish(const char*) {}
void runhist_forget() {}
void runhist_apply_pending(const char*) {}
void savefile_on_leaving_house() {}
void classpick_on_run_restart() {}
void lockstep_enter_battle(uint64_t, uint32_t) {}
void lockstep_on_run_restart() {}
void lockstep_owners_write(EnterNodeMsg&) {}
void lockstep_owners_read(const EnterNodeMsg&) {}
uint32_t lockstep_owners_copy(uint64_t*, uint8_t*, uint32_t) { return 0; }
bool lockstep_in_battle() { return false; }
uint32_t meow_new_seed() { return 1; }
void meow_play(uint32_t) { ++meows; }
NetRole net_role() { return NetRole::Host; }
bool net_active() { return true; }
void net_end_battle_checkpoint() {}
void net_clear_battle_checkpoint() {}
uint8_t net_self() { return 0; }
uint8_t net_peer_count() { return 1; }
bool net_peer_ids(uint8_t*, uint8_t) { return false; }
bool net_send_enter_node(const EnterNodeMsg&) { return true; }
bool net_send_choice(const ChoiceMsg& message) { last_choice = message; return true; }
bool net_send_enter_node_to(uint8_t, const EnterNodeMsg&) { return true; }
const Config& config() { static Config value; return value; }
uintptr_t addr_of_call(Call) { return 0; }
uintptr_t addr_of_data(DataSym) { return 0; }
void log_line_lvl(LogLevel, const char*, const char*, ...) {}

static void __fastcall native_click(void* closure, void*, void*, void*) {
    void* target = nullptr;
    memcpy(&target, (uint8_t*)closure + 8, 8);
    uint8_t* marker = nullptr;
    memcpy(&marker, (uint8_t*)g.map + kMap_Marker, 8);
    memcpy(marker + kMarker_AtNode, &target, 8);
    memcpy(marker + kMarker_Selected, &target, 8);
}

int exercise() {
    uint8_t map[0xB0]{}, marker[0x70]{}, a[0x180]{}, b[0x180]{};
    void* nodes[] = {a,b};
    auto put = [](uint8_t* dest, size_t offset, const void* value, size_t n) {
        memcpy(dest+offset,value,n);
    };
    void* marker_ptr = marker;
    void* node_data = nodes;
    uint32_t count = 2;
    uint64_t seed = 123;
    put(map,kMap_Marker,&marker_ptr,8);
    put(map,kMap_NodeCount,&count,4);
    put(map,kMap_NodeData,&node_data,8);
    put(b,kNode_Seed,&seed,8);
    void* source = a;
    put(marker,kMarker_AtNode,&source,8);
    put(marker,kMarker_Selected,&source,8);
    g.map = map;
    g.on = true;
    g.is_client = false;
    o_node_click = native_click;
    if (!follow_map_save_safe()) return 7;
    void* closure[] = {nullptr,b};
    node_click(closure,nullptr,nullptr,nullptr);
    if(saves!=1 || saved_node!=0 || !g.checkpoint_valid) return 1;
    if (follow_map_save_safe()) return 8; // arrived but not entered yet
    // Emulate the native click/walk, then EnterNode. The save must stay on A.
    void* target=b;
    put(marker,kMarker_AtNode,&target,8);
    put(marker,kMarker_Selected,&target,8);
    bool sent = false;
    if (!follow_on_enter_node(map,b,&sent) || !sent) return 6;
    if(saves!=1 || starts!=1 || saved_node!=0 || !g.checkpoint_valid) return 2;
    // A second entry cannot reuse that snapshot or flush the unresolved B.
    take_entry_checkpoint(map,b);
    if(saves!=1 || g.checkpoint_valid) return 3;
    // Direct entry from vote agreement captures A before marker placement.
    put(marker,kMarker_AtNode,&source,8);
    put(marker,kMarker_Selected,&source,8);
    take_entry_checkpoint(map,b);
    if(saves!=2 || saved_node!=0 || !g.checkpoint_valid) return 4;
    // Mid-walk selection is also unsafe to persist.
    put(marker,kMarker_Selected,&target,8);
    if (follow_map_save_safe()) return 9;
    take_entry_checkpoint(map,b);
    if(saves!=2 || g.checkpoint_valid) return 5;
    // An open backpack blocks the native click but still meows on both peers.
    put(marker,kMarker_Selected,&source,8);
    g.host_vote = -1; g.client_vote = -1; g.host_clicks = 0; g.vote_go = false;
    backpack_open = true;
    const unsigned old_saves = saves;
    node_click(closure,nullptr,nullptr,nullptr);
    if (meows != 1 || saves != old_saves || !follow_map_save_safe() ||
        g.host_vote != -1 || g.host_clicks || g.vote_go ||
        strcmp(last_choice.name,"map-click-sound")) return 10;
    follow_on_vote(last_choice,1);
    if (meows != 2 || g.client_vote != -1 || g.vote_go) return 11;
    // Sound arriving after the backpack closes must still never become a vote.
    backpack_open = false;
    follow_on_vote(last_choice,1);
    if (meows != 3 || g.client_vote != -1 || g.vote_go) return 12;
    // Same local behavior when the clicking peer is the client.
    g.is_client = true; backpack_open = true;
    node_click(closure,nullptr,nullptr,nullptr);
    if (meows != 4 || saves != old_saves || g.client_vote != -1) return 13;
    return 0;
}
}
int main() { return mgmp::exercise(); }
