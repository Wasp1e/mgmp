# Resume on the node before an unfinished fight

The pre-battle checkpoint used to call save_adventure from EnterNode. In the
native-click path the marker has already selected and reached the destination
by then. Saving it persists the unfinished fight node, so loading can strand
the party there without starting the fight.

Capture now starts before the accepted native map click. EnterNode consumes
that checkpoint once, appends the existing state and entry frames, and never
flushes the advanced marker over it. The path that enters directly after vote
agreement still saves before place_marker. Both paths require an unchanged
source/selected node distinct from the destination. Reload/session reset clears
the pending identity, including its map pointer, node pointer and seed.

Other mod save flushes are also refused while the marker is walking or has
reached a pending destination. Initial adventure publication is done before
the native click, so it cannot overwrite the earlier save during entry.

test_map_checkpoint compiles the actual follow implementation with a fake
serializer/transport. It exercises the click and EnterNode path, one-shot
consumption, direct entry and refusal to save a moving/unresolved marker.
It does not run the native serializer or load a real campaign.

This changes checkpoints made with the updated DLL. It does not migrate an
existing save already parked on an unfinished battle node. No existing save
files were edited during development. Live exit-to-menu/load testing remains
necessary on both peers.
