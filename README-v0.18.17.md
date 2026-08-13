# OStim Together v0.18.17 — RaceMenu proxy overlays

Diagnostic/fix build for the STR remote-player appearance problem.

Expected log markers:

- `RaceMenuOverlayBridge READY overlayVersion=...`
- `OSTNET PROXY OVERLAY SNAPSHOT phase=START-PRE ...`
- `OSTNET PROXY OVERLAY REGISTER ... hasBefore=... hasAfter=...`
- `OSTNET PROXY OVERLAY REBUILD phase=T100 ...`
- `OSTNET PROXY OVERLAY REBUILD phase=T500 ...`
- `OSTNET PROXY OVERLAY SNAPSHOT phase=T1200-AFTER ...`

Interpretation:

- `hasBefore=0 -> hasAfter=1`: STR clone was never registered in RaceMenu locally; v0.18.17 fixes that registration.
- `ovl>0` at START-PRE then `ovl=0` at T100-BEFORE: OStim/STR startup physically removed the overlay NIF nodes.
- `ovl=0` before rebuild then `ovl>0` after rebuild: RaceMenu can reconstruct the proxy overlay meshes.
- meshes return but makeup/pubis textures are wrong/blank: the proxy lacks the original NiOverride texture state; next step is explicit override-state synchronization.
- OCum may still require explicit network synchronization because its effect is generated on Elir's real local actor, not Player1's dynamic STR proxy.

Implementation note:

- Overlay registration/rebuild uses RaceMenu's queued/deferred path only; no immediate re-entrant overlay rebuild is requested.
- EquipmentLock and DefaultOutfitGuard continue to skip STR player proxies; real NPC behavior is unchanged.
- Wall/furniture positioning logic is unchanged from v0.18.16.

Build fix 2026-08-11: adjusted the SKSE warning logger calls to the CommonLibSSE-NG-compatible warn() API.
