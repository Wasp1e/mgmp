# House shops and shared nights

Replicated NPC office visits now wait until the client's local House modal
closes, including furniture placement. The check runs both when accepting a
queued start and immediately before the native office transition. If the host
leaves before that transition, the queued visit is cancelled. This avoids
opening an office beneath FurnitureBuildingUI while it owns a held piece and
cat visibility. The reported disappearance still needs a two-instance live
regression check after loading the new DLL.

## House purchase inventory lifetime fix (November 26)

The House InventoryDrawers component persists when its modal is closed. Whole
INVSYNC bucket reads assign fresh Equipment IDs, invalidating the IDs held by
its existing item buttons. Checking only InventoryItemBox update timestamps
does not cover this screen or the frame in which it opens.

Full progression/inventory loads now wait for an unobstructed House with no
selected House modal, no cutscene, no active client shop, and verified drawer
rebuild code. The receiver checks this boundary itself, including when
`net_follow` is disabled. Failed allocation while deferring never falls back
to destructive loading under a live screen.

Immediately after bucket replacement, the native drawer rebuild at RVA
`0x210590` reconciles buttons against the new IDs and removes obsolete buttons.
Calling layout/sort refresh at `0x211700` alone is insufficient: it only works
with the existing button list. The rebuild calls that refresh itself, without
opening the modal. Signature validation happens before replacing any bucket.

`test_invsync_house` compiles the actual receiver with fake native bucket calls.
It checks that a snapshot waits while UI is open, newer snapshots supersede it,
all buckets load before the button rebuild, and identical echoes keep existing
items/buttons intact. The executable's disassembly was checked for the rebuild
and ID reconciliation; the reported client crash still needs a live retest.

Protocol 112 routes client House-shop purchases to the host. Requests identify
the campaign day, catalog, stock slot, item type/key/price and expected remaining
quantity. The host checks them against its live shop and uses the native purchase
function. A request requires the same shop to be open on the host; stale,
different-catalog and sold-out requests do not grant items locally.

Host shop clicks are not also replayed as raw client shop clicks. Both players
can click the shop, while NPC conversations remain driven by the host. Stock
counts and the host's purse update while the shop is open. The host's purse
cannot be overwritten by a delayed client balance observation.

Client stock application also invokes the native shelf refresh at RVA
`0x798610`, once after a batch changes quantities. The native purchase callback
calls it at `0x79A967`; the ordinary Shop update does not reconcile shelf
movies/buttons from changed quantities. Updating `ShopItem+0xC` alone left sold
items visible on the client's shelves. The receiver regression test uses a fake
renderer to verify refresh ordering, batching, identical echoes and rejected
day/catalog/item identities. Visible removal still needs a two-instance test.

`HOUSESHOP` records requests, declines, and progression snapshots. The native
`GlobalProgressionData` serializer at RVA `0x1D3B90` writes the `npc_progress`
blob (version 38), including stock, collars and upgrades. A read-only check of
a saved campaign confirmed the version prefix. Full snapshots are held until
the client's office/dialog and inventory UI close, because deserialization can
replace vectors referenced by those screens. Inventory rewards use INVSYNC;
furniture uses the existing record replication. The evening check also compares
the live native progression hash.

The November 22 client log `mgmp_client_20261122-140929.log`, at 14:13:47.605,
declined the shared night because `files tutorial_tokens` was 1118 bytes on the
host and 1069 locally. This was the only counted difference. Tutorial UI
acknowledgements are now logged as an expected difference. This exception does
not exempt live inventory, purse, progression, furniture or cat comparisons.

The evening comparison is processed on a safe House update after pending shop
state, before deciding whether the client can resolve the night. It waits for
open UI to settle within the existing night-start timeout; a real state
mismatch still falls back to the host's morning save.

Validation: build and pure-logic regression tests cover stale/duplicate requests,
last-item contention, changing catalogs/days, and the tutorial-only mismatch.
Native progression application and purchases across each NPC shop still require
the next two-instance live test. The running game has not been restarted or
replaced by this change.

Protocol 113 also closes client daytime menus at the host's night-start message:
House modals use the native manager close path at RVA `0x203C40` (its panel
callback runs before the manager clears its selection), NPC offices use their
normal Exit, and Pause/Settings use Resume/Back. Pending daytime raw inputs,
script lines, script events and cutscene starts are discarded. The mod panel
is hidden and held click/Space/Enter input is released. No Escape keys are
repeated into the upcoming night.

Night commands now carry the night-start epoch. Queued commands from earlier
nights cannot apply to a new screen 1. The delayed client applies at most one
command per update and displays each event for at least 1500 ms and its outcome
for at least 750 ms. A queued close reveals an unseen outcome through the
native callback first, rather than setting its visible flag and closing it in
the same update. Repeated closes are suppressed. Native menu teardown and
multi-night playback still require live validation after restart.
