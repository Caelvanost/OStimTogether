# OStim Together v0.19.6

Follow-up for OCum Ascended 3D overlays that were synchronized correctly but
remained invisible on both players.

## OverlayFix-aware live rendering

The v0.19.5 diagnostics proved that OCum created the overlays and that OStim
Together transported and applied them without loss: all five marked body
nodes, their texture paths and their visible alpha values were present on the
receiving 3D.

OverlayFix can app-cull an unused RaceMenu overlay while it still has the
default texture or zero alpha. When OCum later replaces those properties, the
live mesh can remain culled even though its override state is correct.

v0.19.6 now clears that stale culling state after applying each marked OCum
overlay. The operation is deliberately limited to the exact synchronized
overlay subtree and is repeated by the existing deferred live reapplication
passes. Other RaceMenu overlays and the user's OverlayFix configuration are
left untouched.

The receiver also stops rebuilding an overlay holder that already exists.
Those repeated rebuilds could replace or re-cull the geometry immediately
after its properties had been applied. A holder is now installed only when a
dynamic Skyrim Together proxy does not have one yet.

Expected diagnostics:

```text
OSTNET ADDON OVR LOCAL REFRESH ... directApplied=5 unculled=...
OSTNET ADDON OVR APPLY ... directNodes=5 unculled=... installed=0
OSTNET ADDON OVR LIVE REAPPLY ... directNodes=5 unculled=...
```

The proxy pose guard and cum-mesh synchronization are unchanged from
v0.19.5.
