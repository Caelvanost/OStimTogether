# OStim Together

Current development version: **0.28.0**.

The root `VERSION` file is the single source of truth for CMake, DLL startup logs and archive names. Small fixes increment patch; larger feature/architecture work increments minor and resets patch.

OStim Together synchronizes OStim Standalone scenes between Skyrim Together Reborn players. The `strpm` branch uses **STRPluginMessagingAPI only**; there is no UDP fallback.

## 0.28.0

### Free-standing player alignment: synchronize the animated skeleton root, not the STR reference

The 0.27.4 two-client test isolated the remaining free-standing alignment failure more precisely. In the reported scene, **Kahel was mispositioned on Player2**. Kahel is a dynamic Skyrim Together proxy on Player2, while the real Kahel is the local PlayerCharacter on Player1.

The important observation was that Kahel's proxy reference on Player2 was already almost exactly on the shared OStim scene origin:

```text
owner target ~= (675,-3760,64)
Kahel proxy ref ~= (674,-3759,63)
```

So another `SetPosition`, `TranslateTo`, convergence barrier or `Update3DPosition` correction would target the wrong layer. Free-standing OStim scenes commonly place actor references on the same origin; the role-specific visible displacement is carried by the animated skeleton. The missing state is therefore the animation-root transform of the remote player proxy, not its network world position.

0.28.0 adds a dedicated **animated root synchronization layer** for ordinary OStim 7.5b no-furniture/non-wall player scenes.

Each participating client samples the real local PlayerCharacter's:

```text
NPC Root [Root]
```

and transmits only that node's **local translation + local rotation matrix** at approximately 30 Hz.

The receiving client resolves the STRPM sender to its dynamic STR proxy and applies the transform only when that proxy is actually an actor in the currently active local OStim player thread.

```text
real Kahel on Player1
  NPC Root [Root].local
        ↓
ostimtogether.root
        ↓
Kahel STR proxy on Player2
  NPC Root [Root].local
```

The same path works symmetrically for Elir:

```text
real Elir on Player2
        ↓
ostimtogether.root
        ↓
Elir STR proxy on Player1
```

### Two separate ownership layers

0.28.0 deliberately separates network movement from paired-animation rendering:

```text
TESObjectREFR world position
→ owned exclusively by Skyrim Together

NPC Root [Root] local animation transform
→ synchronized by OStim Together during free OStim scenes
```

The root-sync path never calls:

- `Actor::SetPosition`;
- `TESObjectREFR::SetPosition`;
- `TranslateTo` on the proxy;
- `Update3DPosition(true)`;
- any world-position hard snap.

It modifies only the local transform of `NPC Root [Root]`, then recomputes that skeleton subtree's world transforms. This prevents the old STR/OStim position fight while restoring the visual displacement authored by the paired animation.

### Dedicated realtime STRPM channel

Animated root data uses its own STRPM channel:

```text
ostimtogether.root
```

This channel is intentionally **unreliable and unordered** (`flags=0`). Root transforms are latest-state data: if a 33 ms sample is lost, the next sample is more useful than retransmitting the old one. START/NODE/SPEED/STOP and consent continue to use the existing reliable/ordered `ostimtogether` channel unchanged.

Incoming root samples expire after 250 ms. A sender can affect a skeleton only if its resolved STR proxy belongs to the active local OStim player thread, which is the scene-membership guard for this realtime state.

Expected startup logs:

```text
OSTNET ROOT SYNC READY threadsVersion=3 node="NPC Root [Root]" sendMs=33 referenceWrites=0
OSTNET ROOT SYNC TRANSPORT READY channel=ostimtogether.root realtimeFlags=0 reliable=0 ordered=0
```

Expected scene logs:

```text
OSTNET ROOT SYNC ARM thread=0 freeStanding=1 actors=...
OSTNET ROOT SYNC TX thread=0 node=... local=(...) channel=ostimtogether.root referenceWrites=0
OSTNET ROOT SYNC APPLY connection=... proxy=... before=(...) target=(...) after=(...) referenceWrites=0
```

For the Kahel case, the critical verification is that Player1's `ROOT SYNC TX ... local=(...)` is received on Player2 and Player2 reports the same values in `ROOT SYNC APPLY ... target=(...) after=(...)` for Kahel's proxy.

Furniture and wall scenes do **not** use this root-bone synchronization. They retain the established anchored position/pose guards.

## 0.27.4

### Free-standing scenes: never warp the STR proxy animation root

The 0.27.3 runtime logs isolated another no-furniture alignment failure. Actor order and the shared scene center were consistent between Player1 and Player2, and the mirror was no longer recomputing a competing center. The remaining correction was the legacy STR proxy pose guard itself.

On a free-standing START, the guard could force the loaded 3D root onto the reference origin through `Update3DPosition(true)`. That is appropriate for a fixed furniture/wall anchor, but destructive for free-standing paired animations.

0.27.4 therefore removed all full-pose correction from ordinary no-furniture/non-wall scenes:

```text
OStim ChangeNode / START
→ native OStim lockAtPosition()
→ OStim starts the paired animation
→ OStim Together immediately disables the full STR proxy pose guard
→ short delay
→ StopTranslation() only
→ STR owns the remote reference
```

For free scenes OStim Together performs none of the following on an STR proxy:

- `SetPosition()`;
- direct reference-position writes;
- `Update3DPosition(true)`;
- convergence hard-snaps;
- periodic world-pose corrections;
- mirror center reconstruction.

0.28.0 keeps that rule and adds a separate skeleton-local root synchronization layer rather than restoring any world-position correction.

Furniture and wall scenes remain unchanged.

## 0.27.3

### Mirror free-scene alignment: preserve the authoritative owner center

0.27.2 proved that the remaining no-furniture instability was not caused by the remote player moving during consent. Player1/owner convergence reached the OStim pose normally, while Player2/mirror timed out on free START/NODE with a nearly constant error even when Elir stood still.

The mirror had already received the authoritative owner center, but the 0.27.2 convergence barrier independently reconstructed a second center from the mirror's already-animated local PlayerCharacter. 0.27.3 separated the owner and mirror paths. 0.27.4 and 0.28.0 supersede the remaining free-scene world-pose logic entirely.

## 0.27.2

### Idempotent shared SPEED

OStim's `SetSpeed()` replays the current animation and emits a speed event even when the requested speed is already active. In a multi-master scene, replaying an identical incoming `SPEED` could therefore create participant → relay → participant feedback and repeated animation restarts.

Shared speed application is idempotent on both sides:

- a mirror ignores an authoritative `SPEED` that already matches its current speed;
- the relay does not call `SetSpeed()` when a participant requests the already-current speed;
- in that no-op relay case the current speed is still fanned out so additional participants converge without replaying the animation.

Expected logs:

```text
OSTNET MIRROR SPEED NOOP ... reason=already-current
OSTNET COOP CONTROL SPEED NOOP ... reason=already-current fanout=...
```

There is still no owner approval or veto for speed changes; this is duplicate-state suppression only.

## 0.27.1

### Shared-control routing fixes

A stale consent timeout could previously erase the route of a newer scene because OStim commonly reuses `thread=0`. Cleanup now removes a route only when it still belongs to the same session.

`CONTROL_NODE` also now applies the exact requested node through `NavigateToSearchResult()` / `Thread::ChangeNode()` instead of relying on `NavigateToScene()` being reachable from the relay's current transition stage.

```text
participant selects/reaches node X
→ CONTROL_NODE(thread, X)
→ active-scene membership guard only
→ relay applies exact ChangeNode(X)
→ resulting NODE is fanned to participants
```

There is **no owner approval or veto**. The original starter remains only the relay/sequencing point.

## 0.27.0

### Shared native OStim scene control

Every client whose real local PlayerCharacter participates in a synchronized multiplayer scene gets OStim's native SceneMenu.

Shared scene control is **multi-master**. Player1, Player2, and additional accepted participants have equal control rights. The shared protocol currently covers:

```text
CONTROL_NODE
CONTROL_SPEED
CONTROL_STOP
```

Actor alignment-editor offsets and arbitrary OStim Scene Options that invoke local Papyrus functions are not yet declared synchronized state.

## 0.26.2 and earlier alignment work

0.26.2 was the first attempt to stop the long-lived STR proxy pose guard from fighting free-standing animation root motion. The 0.27.x tests progressively removed continuous reference corrections, mirror NPC alignment, competing center reconstruction and full rendered-root world warps. 0.28.0 now handles the remaining visual player displacement explicitly at the skeleton-root layer.

The no-furniture specialization applies to OStim 7.5b / Threads ABI v3, where `getFurnitureObject()` can prove whether the scene has a real furniture anchor. Furniture and wall scenes keep their dedicated handling.

## Consent and Add Actor gate

Direct player-targeted scene starts remain gated before OStim receives the scene-start input:

```text
Aim at remote STR player proxy
→ OStim scene-start key
→ OStim Together consumes the key
→ remote consent
→ Accept
→ OStim resumes its normal Furniture / Add Actor / role / fade pipeline
```

The patched mandatory Core `OSKSE.pex` also suspends OStim's UIExtensions Add Actor callback when a remote STR player is selected, so role selection/fade/thread creation cannot continue before consent. OStim Together's `OSKSE.pex` must win its file conflict against OStim's original file.

## OCum state snapshot

On Accept, the remote client sends its current OCum state before `INVITE_RESPONSE`:

- RaceMenu overlay chunks whose texture contains `CumOverlays`;
- `ocumvagmesh` equipped state;
- `ocumanmesh` equipped state.

The normal scene channel is reliable/ordered, so the initiating client receives this appearance state before scene start resumes.

## Other synchronization features

- targeted STRPM consent and scene traffic;
- exact OStim 7.5 furniture synchronization through Threads ABI v3;
- OStim 7.4c furniture fallback;
- Wall-scene startup handling;
- shared native OStim node/speed/stop control for every accepted player participant;
- root-motion-safe free-scene world-position ownership;
- **free-scene `NPC Root [Root]` realtime synchronization in 0.28.0**;
- equipment/outfit protection and residual apparel restoration;
- RaceMenu/SKEE overlay rebuild support;
- generic addon state reapplication;
- optional live OCum Ascended integration.

## Compatibility

Validated OStim runtime layouts:

- OStim 7.4c — `7.4.0.3`;
- OStim 7.5b — `7.5.0.2`.

The free-scene root sync requires Threads API v3 and therefore runs on the validated OStim 7.5b path. OStim 7.4c keeps the older compatibility path.

The required `OSKSE.pex` compatibility patch is based on the OStim 7.5b `OSKSE.psc` interface. Revalidate it when updating OStim.

## Build

First compile the mandatory Add Actor Papyrus compatibility patch only when its Papyrus sources change:

```powershell
.\compat\OStimUIConsent\compile-ui-consent.ps1 `
  -SkyrimDir "C:\Games\Steam\steamapps\common\Skyrim Special Edition"
```

This must produce:

```text
compat\OStimUIConsent\package\Data\Scripts\OSKSE.pex
compat\OStimUIConsent\package\Data\Scripts\OStimTogetherNative.pex
```

Both `build-vortex.ps1` and `build-fomod.ps1` treat those files as mandatory Core inputs.

For **0.28.0 no Papyrus source changed**, so existing compiled PEX files can be reused.

Then build the FOMOD:

```powershell
$env:VCPKG_ROOT="C:\dev\vcpkg"
.\build-fomod.ps1
```

Expected output:

```text
dist\OStimTogether-v0.28.0-FOMOD.zip
```

FOMOD layout:

- `00 Core` — required; includes DLL, INI, `OSKSE.pex` and `OStimTogetherNative.pex`;
- `10 OCum Ascended` — optional live OCum integration.
