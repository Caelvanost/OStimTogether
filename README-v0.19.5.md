# OStim Together v0.19.5

Two-player follow-up for the remaining symmetric STR proxy oscillation.

## Per-frame STR proxy pose authority

The v0.19.4 guard detected the correct OStim pose, but its virtual
`Actor::SetPosition()` call was neutralized on STR's dynamic references. The
test log proved this because every correction reported an unchanged reference
position immediately after the call.

v0.19.5 uses the non-virtual `TESObjectREFR` position path, updates the loaded
3D root from that reference, and runs through a frame-coalesced game-thread
task. There is never more than one pending refresh, even when a frame is slow.

The same guard now covers both synchronized views:

- the remote proxy in the authoritative local OStim thread;
- the sender proxy in the receiving mirror thread, using the transmitted
  authoritative actor pose.

It remains scoped to the active OStim thread and is removed at STOP, returning
normal transform ownership to Skyrim Together outside the scene.

Expected diagnostics:

```text
OSTNET STR PROXY POSE GUARD armed ... mirror=0|1
OSTNET STR PROXY POSE GUARD ... refAfter=... rootAfter=... maxRefAfterDist2=... maxRootAfterDist2=...
```

Both `maxRefAfterDist2` and `maxRootAfterDist2` should be zero or extremely
close to zero.

## OCum overlay diagnostics

The v0.19.4 test did create and transmit OCum overlays: the logs reached four
marked overlay slots. Capture logs now include the selected texture paths, and
receiver logs report texture-property and visible-alpha counts. This makes a
RaceMenu rendering failure distinguishable from an OCum random roll that did
not create an overlay.

The overlay synchronization algorithm and cum mesh synchronization are
otherwise unchanged.
