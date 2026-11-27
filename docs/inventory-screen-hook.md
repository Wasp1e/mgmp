# Inventory screen tracking

The client log `mgmp_client_20261127-191740.log` reports at 19:17:40.987 that
the inventory screen moved and gear synchronization is disabled. At 19:20:06
the client opens a two-cat collar chooser, but never follows into gear.

The inventory tracker had overwritten InventoryScreen2 vtable slot 10 before
classpick verified it against RVA 0x34D130. This disabled classpick's inventory
update hook. A separate lifetime flag could also remain open for a retained
screen and block map clicks after closing the backpack.

Classpick now exclusively owns that slot and reports its screen updates to
invsync. The tracker expires after 300 ms without updates, just like the item
box fallback, and installs no competing screen or destructor hooks. Tests cover
an active screen without item boxes and expiry after it stops updating.

The updated DLL must be loaded after restarting both games. Two-instance live
validation of collars-to-gear and backpack-close-to-map-click remains required.
