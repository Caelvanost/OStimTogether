# OStim Together

Current development version: **0.31.4**.

The root `VERSION` file is the single source of truth for CMake, DLL startup logs and archive names. Small fixes increment patch; larger feature/architecture work increments minor and resets patch.

OStim Together synchronizes OStim Standalone scenes between Skyrim Together Reborn players. The `strpm` branch uses **STRPluginMessagingAPI only**; there is no UDP fallback.

## 0.31.4

### Fix experiment: correct the STR proxy origin, never the real local player

The 0.31.3 two-player runtime log established that the authoritative START center itself is now correct, but the remaining visible offset is not an alignment-cache problem. On Player2, the mirror repeatedly reported:

```text
refBefore=(364.086,-3725.784,63.996)
target=(347.950,-3711.959,64.350)
refAfter=(347.950,-3711.959,64.350)
rootBefore=(364.086,-3725.784,63.996)
rootAfter=(364.086,-3725.784,63.996)
```

The old self-origin lock therefore changed only the real local PlayerCharacter's logical `TESObjectREFR` origin. The rendered root stayed at the OStim animation/root-motion position, and the engine restored the reference to that rendered position between ticks. STR consequently kept publishing the already-displaced real-player sample. The remote proxy then evaluated its own paired OStim animation on top of that displaced sample, effectively counting the participant displacement twice.

0.31.4 reverses the ownership boundary:

```text
real local PlayerCharacter
    -> untouched; OStim owns reference + rendered root

STR remote player proxy
    -> STR supplies the incoming sample
    -> OStim Together cancels only the duplicated logical translation
       by holding proxy TESObjectREFR::data.location on the shared center
    -> rendered proxy 3D/root remains untouched and OStim-owned
```

The legacy `FreeSceneSelfOriginLock` class name is retained for source/package stability, but it now writes **remote proxies only**. No `SetPosition`, `TranslateTo`, `Update3DPosition`, skeleton rotation/translation/scale write, or local-player position write is introduced.

Observer-only Player + NPC mirrors remain outside this path because the 0.31.3 runtime test showed they already align correctly with native OStim behavior.

Expected 0.31.4 logs in a Player1 + Player2 scene:

```text
OSTNET PROXY ORIGIN ARM ... source=local-derived|remote-start ...
OSTNET PROXY ORIGIN LOCK ... actor=<remote STR proxy> ...
OSTNET PROXY ORIGIN STATE ... proxies=1 ... localPlayerWrites=0
```

There should be no recurring `OSTNET SELF ORIGIN LOCK` on the real PlayerCharacter.

### Native live OCum mesh watcher

The 0.31.3 tests separated OCum overlays from OCum equip-object meshes:

- 3D/RaceMenu overlays synchronize and render correctly;
- vaginal/anal meshes disappear specifically while their wearer participates in the active OStim scene;
- observer clients can sometimes see the same NPC mesh while the scene owner cannot;
- pre-scene snapshots can publish `vagMesh=0 / analMesh=0`, and no later `equipped=1` packet was emitted in the failing player-player tests;
- `AddonStateRepair` then kept reapplying that stale false state to the remote player proxy after node/body rebuilds.

0.31.4 adds a native watcher independent of the optional Papyrus event timing. While OStim threads are active it polls every 100 ms and reads the actual OCum armor forms used by the two OStim equip objects:

```text
OCum.esp:00000F37 -> ocumvagmesh
OCum.esp:00000F3B -> ocumanmesh
```

The watcher does not infer a mesh from an overlay or climax type. It acts only when the corresponding armor is really `IsWorn()`.

For the true local PlayerCharacter, every live mesh transition replaces the previous ADDONOBJ cache immediately:

```text
actual worn state 0 -> 1
-> ADDONOBJ ... equipped=1
-> remote cache becomes true
-> AddonStateRepair re-applies true, not the stale pre-scene false
```

For local non-STR-proxy wearers, including ordinary NPC scene actors, the watcher also requests a rate-limited `Actor.QueueNiNodeUpdate` every 500 ms while an OCum mesh remains worn. This is the same Skyrim 3D rebuild mechanism OStim itself uses after equipping an equip object; the repeated refresh is intended to survive later scene-time body/node rebuilds without continuously unequipping/re-equipping the object.

A delayed post-STOP snapshot publishes the final real-player OCum state after OStim cleanup settles.

No Papyrus source changed in 0.31.4, so an already-current 0.31.0+ `OStimTogetherOCum.pex` does not need recompilation.

## 0.31.3

### Fix: remote participant uses the owner's START center

The 0.31.2 three-actor runtime test exposed a second free-scene origin bug. Player1 correctly transmitted the authoritative START center, but Player2's mirror later recomputed another center from Player2's own local role position. Because the role-specific graph offset available to that path was zero, the two clients armed different logical origins and STR published Player2 from the wrong point on both machines.

0.31.3 makes the START packet authoritative for mirror clients:

```text
owner START center
→ StartRemoteMirror(... authoritativeCenter ...)
→ OStimBridge::_sceneCenters[localMirrorThread]
→ FreeSceneSelfOriginLock
```

A locally-owned multiplayer scene still derives its center after OStim startup. A remote mirror never derives a second center; it reads the already-stored owner center.

### Fix: OCum equipment is exempt from NPC anti-reequip locking

The runtime logs also showed the NPC `EquipmentLock` repeatedly calling `UnequipObject()` on worn scene equipment every 25 ms. OCum Ascended itself uses armor records from `OCum.esp` both as short-lived RaceMenu overlay bootstrap helpers and as persistent OStim equip-object meshes.

0.31.3 excludes every armor whose defining file is `OCum.esp` from `EquipmentLock`. This is intentionally plugin-origin based rather than a hard-coded FormID list, so OCum's current and future runtime helper armors remain under OCum's ownership while normal NPC clothing continues to be governed by `SlotMask`.

## 0.31.2

### Fix: Player + NPC scene freeze during OStim START

0.31.1 introduced `FreeSceneSelfOriginLock` to keep each real multiplayer participant's logical `TESObjectREFR` origin on the shared free-scene center while OStim remains responsible for rendered animation/root motion.

The first implementation armed that system synchronously from OStim's `START` listener. Arming calls `OStimBridge::TryComputeSceneCenter()`, which uses OStim's `GetActorAlignment()` ModAPI path. OStim emits `START` while its own thread startup is still in progress, so re-entering thread-control/alignment state from that callback can stall the game before startup returns.

0.31.2 changes the startup path in two ways:

1. **Player + NPC scenes bypass the shared-origin system entirely.**
2. **Real multiplayer scenes arm through a two-hop game-task defer.** The first hop yields back to OStim so its initial `ChangeNode()` can enqueue `lockAtPosition()` work; the second hop runs after those startup tasks and only then reads alignment/scene-center state.

## 0.31.0

### Free-standing alignment: each real player owns their OStim alignment

The same human participant is represented by two different Skyrim actors in multiplayer:

```text
Player2 client: real Elir PlayerCharacter
Player1 client: dynamic STR proxy representing Elir
```

OStim derives an alignment key from actor characteristics and resolves `ActorAlignmentData` from that key. A dynamic STR proxy is not guaranteed to resolve the same alignment key/cache entry as the real PlayerCharacter it represents.

0.31.0 adds a second reliable/ordered STRPM channel:

```text
ostimtogether.align
```

For every ordinary no-furniture/non-wall node:

```text
real local PlayerCharacter
→ OStim GetActorAlignment(self)
→ ALIGN_STATE(node, offsetX/Y/Z, scale, rotation, sosBend)
→ remote client resolves sender's STR proxy
→ OStim SetActorAlignment(proxy, participant-authored values)
→ existing clock-calibrated phase replay
```

Authority is symmetric:

- Kahel's real PlayerCharacter is authoritative only for Kahel's OStim alignment;
- Elir's real PlayerCharacter is authoritative only for Elir's OStim alignment;
- additional participants follow the same rule.

This path performs no skeleton write and uses OStim's public `SetActorAlignment()` once per node. Furniture and wall scenes are excluded and keep their established anchored handling.

### OCum equip-object mesh detection

`ocumvagmesh` and `ocumanmesh` are OStim equip-object types. The optional OCum integration reads both OStim's object state and the underlying armor equipped state:

```text
OActor.IsObjectEquipped(Target, type)
OR
Target.IsEquipped(the corresponding OCum armor)
```

The 0.31.4 native watcher supplements this optional event-based path with direct runtime worn-state observation.

## 0.30.5

### Clock-calibrated free-scene phase replay

Ordinary no-furniture/non-wall scenes use the reliable/ordered `ostimtogether.phase` channel:

```text
owner thread/node becomes active
→ PHASE_PREP
→ remote waits until its real local mirror is ready
→ PHASE_READY with NTP-like timing sample
→ owner chooses a future execution deadline
→ PHASE_COMMIT
→ every client replays through the synchronized animation path
```

A delayed native `StopTranslation` clears transient OStim `TranslateTo` on dynamic STR proxies.

## 0.28.1 safety rule

The 0.28.0 experiment that copied `NPC Root [Root]` transforms between independently evaluated skeletons could catastrophically deform an actor. That mechanism remains disabled.

Safety invariant:

```text
remote skeleton rotation writes = 0
remote skeleton scale writes = 0
recursive skeleton transform rebuild = 0
```

The later translation-only root probe also remains disabled because tested nodes exposed no useful local root delta.

## Free-standing world-position ownership

For ordinary shared free-standing scenes:

- OStim owns the real local PlayerCharacter reference, rendered root and animation;
- STR remains the source of incoming remote-player movement samples;
- OStim Together may continuously correct only the **remote proxy's logical `TESObjectREFR::data.location`** to the common scene center so STR-transmitted root-motion displacement is not counted twice;
- OStim Together does not directly move the proxy 3D or write its skeleton transforms.

Furniture and wall scenes retain their dedicated anchored paths.

## Shared scene control

Every accepted player participant gets OStim's native SceneMenu. Scene control is multi-master and synchronizes:

```text
CONTROL_NODE
CONTROL_SPEED
CONTROL_STOP
```

Player1, Player2 and additional accepted participants have equal control rights. The original initiator is only a deterministic relay/sequencing point; there is no owner approval or veto.

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

## Other synchronization features

- targeted STRPM consent and scene traffic;
- exact OStim 7.5 furniture synchronization through Threads ABI v3;
- OStim 7.4c furniture fallback;
- wall-scene startup handling;
- shared native OStim NODE/SPEED/STOP controls;
- clock-calibrated free-standing START/NODE phase barrier;
- participant-authored OStim alignment over `ostimtogether.align`;
- safe orphan proxy-only auxiliary OStim thread cleanup;
- remote-proxy logical-origin correction for shared free scenes;
- equipment/outfit protection with OCum.esp runtime armor exemption;
- native live OCum mesh-state tracking and local 3D refresh;
- RaceMenu/SKEE overlay rebuild support;
- generic addon state reapplication;
- optional OCum Ascended event-based overlay/equip-object integration.

## Compatibility

Validated OStim runtime layouts:

- OStim 7.4c — `7.4.0.3`;
- OStim 7.5b — `7.5.0.2`.

The free-scene phase/alignment specialization requires Threads API v3 for exact furniture exclusion and therefore targets the validated OStim 7.5b path. Furniture/wall handling remains separate.

The required `OSKSE.pex` compatibility patch is based on the OStim 7.5b `OSKSE.psc` interface. Revalidate it when updating OStim.

## Build

The mandatory Add Actor Papyrus patch did not change in 0.31.4.

The optional OCum Papyrus integration also did not change in 0.31.4. Recompile it only if your local packaged `OStimTogetherOCum.pex` is not already the 0.31.0-or-newer build:

```powershell
.\optional\OCumIntegration\compile-ocum-integration.ps1 `
  -SkyrimDir "C:\Games\Steam\steamapps\common\Skyrim Special Edition"
```

Then build the FOMOD:

```powershell
$env:VCPKG_ROOT="C:\dev\vcpkg"
.\build-fomod.ps1
```

Expected output:

```text
dist\OStimTogether-v0.31.4-FOMOD.zip
```

FOMOD layout:

- `00 Core` — required DLL/INI plus mandatory Add Actor gate scripts;
- `10 OCum Ascended` — optional OCum integration with `OStimTogetherOCum.pex`.
