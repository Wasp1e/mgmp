#include "mgmp_house_drag.h"
#include <cstdio>
#include <limits>

using namespace mgmp;
static int failures = 0;
static void check(bool ok, const char* name) {
    std::printf("%s %s\n", ok ? "ok" : "FAIL", name);
    if (!ok) ++failures;
}

int main() {
    HouseDragTracker t;
    check(t.sample(1, 42, 1, 1.0f, 2.0f, 100, 0), "first sample acquires cat");
    check(t.slot(1).key == 42 && t.slot(1).x == 1.0f, "sample stored");
    check(!t.sample(1, 42, 1, 3.0f, 4.0f, 101, 0), "duplicate rejected");
    check(!t.sample(2, 42, 1, 3.0f, 4.0f, 102, 0), "another peer cannot own same cat");
    check(!t.release(2, 42, 2), "non-owner cannot release cat");
    check(!t.sample(2, 9, 3, 0, 0, 103, 9), "local hand takes priority");
    check(t.sample(1, 42, 2, 5, 6, 104, 0), "newer sample accepted");
    check(!t.release(1, 42, 2), "stale release rejected");
    check(t.release(1, 42, 3) && !t.slot(1).key, "owner releases cat");
    check(t.sample(2, 42, 4, 3, 4, 105, 0), "another peer can acquire released cat");
    t.exclude_local(42);
    check(!t.slot(2).key, "local pickup cancels remote lease");
    check(t.sample(2, 42, 5, 3, 4, 106, 0), "peer can acquire after local release");
    t.expire(106 + HouseDragTracker::kLeaseMs + 1);
    check(!t.slot(2).key, "lease expires after missing updates");
    check(!t.sample(2, 42, 5, 3, 4, 2000, 0), "expired old sample still rejected");
    t.reset();
    check(t.sample(2, 42, 1, 3, 4, 2001, 0), "new session resets sequence");
    check(!t.sample(HouseDragTracker::kNoPeer, 42, 1, 0, 0, 2002, 0), "invalid peer rejected");
    check(!t.sample(0, 42, 1, 0, 0, 2002, 0), "self message rejected");
    t.reset();
    check(t.sample(2, 42, 1, 0, 0, 0, 0, 3), "lower priority peer arrives first");
    check(t.sample(0, 42, 1, 2, 3, 1, 0, 3), "host preempts remote owner");
    check(t.owner(42) == 0 && !t.slot(2).key, "only highest priority owner remains");
    check(!t.sample(2, 42, 2, 0, 0, 2, 0, 3), "loser cannot re-acquire while host holds");
    t.exclude_local(42, 3);
    check(t.owner(42) == 0, "local pickup does not evict higher priority peer");
    t.reset();
    check(t.sample(0, 42, 1, 0, 0, 0, 42, 1), "host beats simultaneous local client pickup");
    check(!t.sample(1, 9, 1, 0, 0, 0, 9, 0), "client cannot beat simultaneous host pickup");
    check(!t.sample(2, 9, 1, std::numeric_limits<float>::quiet_NaN(), 0, 0, 0, 1),
          "NaN position rejected");
    check(!t.sample(2, 9, 1, 0, std::numeric_limits<float>::infinity(), 0, 0, 1),
          "infinite position rejected");
    check(!t.sample(2, 9, 1, 10001, 0, 0, 0, 1), "out of range position rejected");
    return failures ? 1 : 0;
}
