# OStim Together

Current development version: **0.30.4**.

The root `VERSION` file is the single source of truth for CMake, DLL startup logs and archive names. Small fixes increment patch; larger feature/architecture work increments minor and resets patch.

OStim Together synchronizes OStim Standalone scenes between Skyrim Together Reborn players. The `strpm` branch uses **STRPluginMessagingAPI only**; there is no UDP fallback.

## 0.30.4

### Free-standing alignment: use OStim's own replay path

The 0.30.3 two-client logs showed that the clock-calibrated phase barrier itself was active, but its direct animation-event replay was not actually accepted by either actor. Every synchronized replay reported:

```text
OSTNET PHASE REPLAY ... mode=direct-animation-event directNotify=0/2 notifyFailures=2
```

for both Player1 and Player2, across every tested free-standing node. The scene center and participant order were already consistent, but the phase correction therefore never restarted the paired animations through a working path.

0.30.4 keeps the same `ostimtogether.phase` PREP/READY/COMMIT timing barrier but forces replay through OStim's public ModAPI:

```text
Thread::SetSpeed(currentSpeed)
```

This is the same native path OStim uses itself:

```text
Thread::SetSpeed
→ ThreadActor::playAnimation
→ GameActor::playAnimation
→ queued NotifyAnimationGraph
```

It naturally preserves OStim's role animation index, playback speed and `singleSpeed` handling. The phase replay still performs **no** `SetActorAlignment`, direct `SetPosition`, `Update3DPosition`, or skeleton-transform write.

Expected 0.30.4 phase log:

```text
OSTNET PHASE REPLAY ... mode=fallback-set-speed fallbackSetSpeed=1 fallbackResult=0 fallbackReason=actor-0-event-probe-native-ostim-phase-replay
```

The fallback label is historical: in 0.30.4 this is the intentionally selected replay path.

The 0.30.3 `NPC Root [Root]` translation channel is also disabled at runtime. Both clients reported `local=(0,0,0)` throughout the tested scenes, so it carried no useful alignment delta. 0.30.4 performs no root-translation writes.

Furniture and wall scenes are unchanged.

## 0.29.0

### Free-standing alignment: synchronize animation phase, not remote transforms

The 0.28.1 two-client logs isolated a timing problem that world-position and skeleton-root corrections could not solve safely.

On the authoritative client, the local OStim thread is already alive and animating before the mirror client's real OStim thread is fully constructed. That phase offset matters for ordinary no-furniture root-motion animations even when both clients agree on the world-space scene origin.

0.29.0 introduced a dedicated reliable/ordered phase channel:

```text
ostimtogether.phase
```

For free-standing START and later NODE changes:

```text
owner OStim thread/node becomes active
→ PHASE_PREP(token, node)
→ each remote waits until its real local OStim mirror exists on that node
→ PHASE_READY
→ owner waits for every participant
→ PHASE_COMMIT(current speed, calibrated future deadline)
→ every client replays the current OStim animation at that deadline
→ short OStim TranslateTo on STR proxies is released with StopTranslation
```

The owner is only the phase coordinator. This does **not** add approval or veto semantics to the multi-master controls.

The first `NODE` event emitted by OStim during thread construction occurs before the real `thread START`. The phase component tracks started player threads and ignores those pre-start NODE callbacks so an incomplete initial thread cannot create a false phase generation.

Furniture and wall scenes are intentionally excluded and retain their established anchored handling.

## 0.28.1

### Safety rollback: remote skeleton writes disabled

0.28.0 experimented with copying the remote player's `NPC Root [Root]` local transform into the corresponding STR proxy. Runtime testing showed that writing translation/rotation into an independently evaluated skeleton and recursively rebuilding the subtree could catastrophically deform the actor.

0.28.1 disabled that experiment. The safety rule remains:

```text
remote skeleton rotation writes = 0
remote skeleton scale writes = 0
recursive skeleton transform rebuild = 0
```

0.30.4 additionally disables the later translation-only experiment because its measured value was always zero in the tested free-standing scenes.

## 0.27.4

### Free-standing scenes: never warp the STR proxy 3D root

Earlier logs showed that the legacy full proxy pose guard could call `Update3DPosition(true)` after OStim had begun a free-standing paired animation. That reset the rendered 3D root onto the reference and destroyed animation-relative displacement.

Ordinary no-furniture/non-wall scenes therefore do not use the full proxy pose guard after startup/node transition. OStim Together performs no periodic free-scene world-position correction. Skyrim Together owns the remote reference; OStim owns its native animation/alignment work.

Furniture and wall scenes retain their anchored guards.

## 0.27.2–0.27.3

### SPEED idempotence and mirror-center fixes

Shared SPEED application is idempotent. OStim's `SetSpeed()` replays an animation even when the requested speed already matches the current speed, so identical incoming values are suppressed to avoid feedback/replay loops.

The mirror also preserves the authoritative owner scene center instead of recomputing a competing center from an already animated local player.

## 0.27.1

### Shared-control routing fixes

A stale consent timeout can no longer erase a newer route that reused the same OStim thread ID.

`CONTROL_NODE` applies the exact requested node through `NavigateToSearchResult()` / `Thread::ChangeNode()` instead of relying on navigation reachability from the relay's current transition stage.

There is **no owner approval or veto**. The original starter is only a deterministic relay/sequencing point.

## 0.27.0

### Shared native OStim scene control

Every accepted player participant gets OStim's native SceneMenu. Scene control is multi-master and currently synchronizes:

```text
CONTROL_NODE
CONTROL_SPEED
CONTROL_STOP
```

Player1, Player2 and additional accepted participants have equal control rights.

## Consent and Add Actor gate

Direct player-targeted starts are gated before OStim receives the scene-start input:

```text
aim at remote STR proxy
→ OStim scene-start key
→ OStim Together consumes the key
→ remote consent
→ Accept
→ OStim resumes Furniture / Add Actor / role / fade setup
```

The mandatory Core `OSKSE.pex` patch also suspends OStim's UIExtensions Add Actor callback when a remote STR player is selected. OStim Together's `OSKSE.pex` must win its conflict against OStim's original file.

## OCum state snapshot

On Accept, the remote client sends its current OCum state before `INVITE_RESPONSE`:

- RaceMenu overlay chunks whose texture contains `CumOverlays`;
- `ocumvagmesh` equipped state;
- `ocumanmesh` equipped state.

## Other synchronization features

- targeted STRPM consent and scene traffic;
- exact OStim 7.5 furniture synchronization through Threads ABI v3;
- OStim 7.4c furniture fallback;
- Wall-scene startup handling;
- shared native OStim NODE/SPEED/STOP controls;
- clock-calibrated free-standing START/NODE phase barrier;
- native OStim phase replay through `SetSpeed(currentSpeed)` in 0.30.4;
- proxy-only auxiliary OStim thread cleanup;
- equipment/outfit protection and residual apparel restoration;
- RaceMenu/SKEE overlay rebuild support;
- generic addon state reapplication;
- optional live OCum Ascended integration.

## Compatibility

Validated OStim runtime layouts:

- OStim 7.4c — `7.4.0.3`;
- OStim 7.5b — `7.5.0.2`.

The free-scene phase barrier requires Threads API v3 for exact furniture exclusion and therefore targets the validated OStim 7.5b path. Furniture/wall paths remain unchanged.

The required `OSKSE.pex` compatibility patch is based on the OStim 7.5b `OSKSE.psc` interface. Revalidate it when updating OStim.

## Build

Compile the mandatory Add Actor Papyrus compatibility patch only when its Papyrus sources change:

```powershell
.\compat\OStimUIConsent\compile-ui-consent.ps1 `
  -SkyrimDir "C:\Games\Steam\steamapps\common\Skyrim Special Edition"
```

This must produce:

```text
compat\OStimUIConsent\package\Data\Scripts\OSKSE.pex
compat\OStimUIConsent\package\Data\Scripts\OStimTogetherNative.pex
```

No Papyrus source changed for **0.30.4**, so existing compiled PEX files can be reused.

Build the FOMOD:

```powershell
$env:VCPKG_ROOT="C:\dev\vcpkg"
.\build-fomod.ps1
```

Expected output:

```text
dist\OStimTogether-v0.30.4-FOMOD.zip
```

FOMOD layout:

- `00 Core` — required; DLL, INI, `OSKSE.pex`, `OStimTogetherNative.pex`;
- `10 OCum Ascended` — optional live OCum integration.
