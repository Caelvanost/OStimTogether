# OStim Together v0.18.15 — Wall mirror boot reassert

Based on v0.18.14.

## Change
For mirrored Wall starts only, queue an authoritative SELF one-shot directly from the mirror thread START listener. The helper uses its existing two-hop SKSE defer, so the correction lands after OStim's initial ChangeNode/lockAtPosition startup alignment. The existing post-initial-animation-replay START-WALL correction remains as a second guard.

Expected receiver log:
- `OSTNET WALL MIRROR BOOT SELF REASSERT queued ...`
- `OSTNET AUTH SELF ONESHOT reason=START-WALL-BOOT ...`
- later `OSTNET WALL START SELF REASSERT queued ...`
- later `OSTNET AUTH SELF ONESHOT reason=START-WALL ...`

No changes to exact furniture, STR ownership, EquipmentLock, or the v0.18.14 STR proxy DefaultOutfitGuard fix.
