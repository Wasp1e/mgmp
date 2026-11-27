#include "mgmp_house_shop_rules.h"
#include <cstdio>
using namespace mgmp;
static int failures = 0;
static void check(bool ok, const char* name) {
    std::printf("%s %s\n", ok ? "ok" : "FAIL", name);
    if (!ok) ++failures;
}
int main() {
    check(house_shop_request_current(50, 203, 203, 10, 10, 2, 2),
          "client can request an available item from the matching shop");
    // Host has bought it before the client's echoed click/request arrives.
    check(!house_shop_request_current(50, 203, 203, 10, 10, 1, 2),
          "host purchase followed by its mirrored client click cannot buy twice");
    check(!house_shop_request_current(50, 203, 203, 10, 10, 0, 1),
          "simultaneous last-item requests cannot grant a sold-out item");
    check(!house_shop_request_current(50, 204, 203, 10, 10, 2, 2),
          "previous-day request cannot spend today's purse");
    check(!house_shop_request_current(50, 203, 203, 20, 10, 2, 2),
          "switching shops rejects a queued purchase from the old catalog");
    check(!house_shop_request_current(2001, 203, 203, 10, 10, 2, 2),
          "abandoned request expires before reopening the shop");
    check(house_local_ui_row("files tutorial_tokens"),
          "November 22 tutorial-only difference does not block the shared night");
    check(!house_local_ui_row("files npc_progress") &&
          !house_local_ui_row("properties house_gold") &&
          !house_local_ui_row("cats 3"),
          "UI exception does not exempt purchases, currency or cats");
    return failures ? 1 : 0;
}
