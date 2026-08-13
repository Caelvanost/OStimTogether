# OStim Together v0.18.12 — Wall START SELF reassert

Based on v0.18.11 furniture-preanchor.

## Change

Wall starting nodes have no exact TESFurniture reference. On the mirror client, OStim can move the local PlayerCharacter slightly away from the sender START pose during the initial SetSpeed()/playAnimation alignment.

v0.18.12 keeps all exact-furniture behavior unchanged and, only for starting nodes whose ID contains `wall`, queues one authoritative SELF pose correction after the initial animation replay.

Expected Player2 log:

```text
OSTNET WALL START SELF REASSERT queued ...
OSTNET AUTH SELF ONESHOT reason=START-WALL ...
```

Build with `build-vortex.ps1`. Output: `OStimTogether-v0.18.12-Vortex.zip`.
