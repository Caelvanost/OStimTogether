# OStim Together v0.18.13 — delayed authoritative Wall START

Based on v0.18.12.

## Why

OStim Wall starts have no TESFurniture reference. Logs from v0.18.12 showed that the generic two-task START snapshot was still provisional: Player1 transmitted one position, while the first later Wall NODE used a substantially different settled scene anchor. Reasserting Player2 to the START snapshot therefore reasserted the wrong transform.

## Change

Only authoritative starting nodes whose ID contains `wall` are changed:

- the ordinary two-hop post-align point is still reached;
- instead of transmitting START immediately, the plugin queues it for 1000 ms;
- `VisualKeepAlive` fires the delayed START on Skyrim's game thread;
- center/actor poses are recomputed at that time and only then transmitted;
- Player2 creates the mirror from that delayed authoritative snapshot;
- the v0.18.12 one-shot `START-WALL` reassert remains, but now targets the delayed pose.

Furniture scenes and ordinary no-furniture scenes keep the existing v0.18.11/v0.18.12 paths.

## New diagnostics

Player1 should log:

- `OSTNET WALL START DELAY queued ... earlyCenter=(...)`
- about one second later: `OSTNET WALL START DELAY fire ... delayed=(...) delta2=...`
- then the normal `OSTNET|v1|START...`

Player2 should then receive that delayed START and later log:

- `OSTNET WALL START SELF REASSERT queued ...`
- `OSTNET AUTH SELF ONESHOT reason=START-WALL ...`

The most important diagnostic is `delta2` on Player1. A large value proves that OStim's Wall transform was still moving after the previous two-hop capture.

## Build

Run `build-vortex.ps1` as before. Output:

`OStimTogether-v0.18.13-Vortex.zip`
