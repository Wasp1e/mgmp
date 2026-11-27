#include "mgmp_night_playback.h"
#include <cstdio>
using namespace mgmp;
static int failures = 0;
static void check(bool ok, const char* name) {
    std::printf("%s %s\n", ok ? "ok" : "FAIL", name);
    if (!ok) ++failures;
}
int main() {
    NightPlaybackGate g;
    g.reset(100);
    check(g.accepts(100) && !g.accepts(99) && !g.accepts(0), "only the current night's commands are accepted");
    g.begin_scene(1000);
    check(!g.can_show(1000) && !g.can_close(1000), "queued clicks cannot skip a newly opened screen");
    check(g.can_show(1350) && !g.can_close(2500), "a close command must first reveal an unseen outcome");
    g.observe_outcome(2500);
    check(!g.can_close(2500) && !g.can_close(3249) && g.can_close(3250),
          "a delayed close keeps the revealed outcome visible for 750 ms");
    g.observe_outcome(3000);
    check(g.can_close(3250), "subsequent frames do not restart the outcome timer");
    g.closed = true;
    check(!g.can_close(4000) && !g.can_show(4000), "duplicate commands cannot close a screen twice");
    g.begin_scene(5000);
    g.observe_outcome(5000);
    check(!g.can_close(5750) && g.can_close(6500), "each new event gets its own minimum display time");
    g.reset(200);
    check(!g.accepts(100) && g.accepts(200) && !g.closed && !g.outcome_seen,
          "the next night rejects old screen-one clicks and resets playback state");
    return failures ? 1 : 0;
}
