# OStim Together v0.18.16 — STR proxy equipment-lock diagnostic + Wall root probe

This build keeps the working v0.18.13 delayed authoritative Wall START and exact-furniture behavior.

Changes:

- Automatic `EquipmentLock` now **skips Skyrim Together remote-player proxies** (dynamic `FF...` reference + dynamic `FF...` TESNPC base). Real NPCs keep the old lock. This is a causal diagnostic for remote-player makeup / RaceMenu visual / OCum disappearance. Clothing may therefore reappear on the remote player during this diagnostic build.
- Removes the ineffective v0.18.15 `START-WALL-BOOT` reassert. The post-initial-replay `START-WALL` one-shot remains.
- Adds a read-only 1.5 s Wall root probe on the receiver, logging `TESObjectREFR` position versus rendered `NiAVObject::world.translate` every ~50 ms. No extra movement is applied.
- `AUTH SELF ONESHOT` logs now include rendered-root positions before/after the correction.

Expected diagnostic lines:

```text
EquipmentLock SKIP STR proxy thread=... actor=FF...... base=FF......
OSTNET WALL ROOT PROBE armed ...
OSTNET WALL ROOT PROBE ... ref=(...) root=1(...) target=(...) refTargetDist2=... rootRefDist2=... rootTargetDist2=...
OSTNET AUTH SELF ONESHOT reason=START-WALL ... rootBefore=... rootAfter=... rootTargetDist2=...
```

Test the same Wall scene and a climax. Report whether the remote player's clothes reappear, whether makeup remains, whether OCum appears, and provide both logs.
