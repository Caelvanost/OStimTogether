# OStim Together

Current development version: **0.31.5**.

The root `VERSION` file is the single source of truth for CMake, DLL startup logs and archive names. Small fixes increment patch; larger feature/architecture work increments minor and resets patch.

OStim Together synchronizes OStim Standalone scenes between Skyrim Together Reborn players. The `strpm` branch uses **STRPluginMessagingAPI only**; there is no UDP fallback.

## 0.31.5

### OCum: poll OStim's live equip-object state for the whole scene

The 0.31.4 Player1 + Player2 runtime test established two important facts:

- Player2 could see the cum mesh during the scene even though the native F37/F3B inventory probe reported `vagMesh=0 analMesh=0` for the entire thread;
- by scene end Player2 had five valid `CumOverlays` RaceMenu slots, but no live `ADDON OVR` packet had been emitted while the scene was running.

The backing armor is therefore not a reliable scene-time representation of an OStim equip object, and `ocum_applied_cum` is not reliable enough as the only synchronization trigger in a mirrored multiplayer scene.

The optional `OStimTogetherOCum` integration now registers for OStim's standard thread-0 `ostim_start` and `ostim_end` events. While the scene is active it polls the TRUE local PlayerCharacter every 500 ms. Each poll:

```text
OVR|ocum|CumOverlays
-> Core captures current marked RaceMenu properties
-> Core refreshes the same live local overlay geometry
-> properties are sent to the other player

OActor.IsObjectEquipped(player, "ocumvagmesh" / "ocumanmesh")
OR OCum F37/F3B armor fallback
-> OBJ state is sent to the other player
```

The existing `ocum_applied_cum` bounded probes remain for low latency. The scene poll is the robust fallback and never treats an STR proxy as an appearance authority.

Because the Papyrus source changed, **0.31.5 requires recompiling `OStimTogetherOCum.pex` before the FOMOD build**.

### Alignment: read-only local/proxy role diagnostics

0.31.4 made both clients converge to the same arrangement, but Elir remained visibly misaligned. The tested OStim `OStim2PMissionaryMF` scene does not define actor position offsets, so the logged `graphOffset=(0,0,0,0)` values are valid and are not an ABI-probe bug.

0.31.5 deliberately does not add another positional/skeleton correction yet. Instead, every 500 ms the existing free-scene diagnostics now report both sides of the ownership boundary:

```text
OSTNET FREE ROLE DIAG ... kind=local-player ...
OSTNET PROXY ORIGIN LOCK ... rootFromCenter=(...) rootScale=...
```

For the real local player and the STR proxy, the log records reference position, rendered root position, rendered root offset from the authoritative center, and rendered root scale. This is read-only. Comparing Player1 and Player2 will tell us whether STR is simply copying OStim's legitimate role/root displacement to the proxy or whether the proxy acquires an additional transform/scale divergence locally.

No new `SetPosition`, `TranslateTo`, `Update3DPosition`, root transform, skeleton transform, or scale write is added by this diagnostic.

## 0.31.4

### Fix experiment: correct the STR proxy origin, never the real local player

The 0.31.3 two-player runtime log established that the authoritative START center itself is now correct, but the remaining visible offset is not an alignment-cache problem. The old self-origin lock changed only the real local PlayerCharacter's logical `TESObjectREFR` origin while the rendered root stayed at the OStim animation/root-motion position.

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

The legacy `FreeSceneSelfOriginLock` class name is retained for source/package stability, but it writes **remote proxies only**. No `SetPosition`, `TranslateTo`, `Update3DPosition`, skeleton rotation/translation/scale write, or local-player position write is introduced.

Observer-only Player + NPC mirrors remain outside this path because the 0.31.3 runtime test showed they already align correctly with native OStim behavior.

### Native OCum armor fallback

0.31.4 added a native watcher for the OCum F37/F3B backing armor and a local `QueueNiNodeUpdate` refresh when that armor is exposed as worn. The 0.31.5 runtime findings show this representation is only a fallback: OStim's own `OActor.IsObjectEquipped()` state is authoritative during a scene when inventory `IsWorn()` remains false.

## 0.31.3

### Fix: remote participant uses the owner's START center

The 0.31.2 three-actor runtime test exposed a second free-scene origin bug. Player1 correctly transmitted the authoritative START center, but Player2's mirror later recomputed another center from Player2's own local role position.

0.31.3 makes the START packet authoritative for mirror clients:

```text
owner START center
→ StartRemoteMirror(... authoritativeCenter ...)
→ OStimBridge::_sceneCenters[localMirrorThread]
→ FreeSceneSelfOriginLock
```

A locally-owned multiplayer scene still derives its center after OStim startup. A remote mirror never derives a second center; it reads the already-stored owner center.

### Fix: OCum equipment is exempt from NPC anti-reequip locking

The runtime logs showed the NPC `EquipmentLock` repeatedly calling `UnequipObject()` on worn scene equipment every 25 ms. OCum Ascended uses armor records from `OCum.esp` both as short-lived RaceMenu overlay bootstrap helpers and as OStim equip-object meshes.

0.31.3 excludes every armor whose defining file is `OCum.esp` from `EquipmentLock`. Ordinary NPC clothing remains governed by `SlotMask`.

## 0.31.2

### Fix: Player + NPC scene freeze during OStim START

0.31.1 introduced `FreeSceneSelfOriginLock`. The first implementation armed synchronously from OStim's `START` listener and could re-enter OStim's thread-control/alignment state while startup was still in progress.

0.31.2 changes startup in two ways:

1. **Player + NPC scenes bypass the shared-origin system entirely.**
2. **Real multiplayer scenes arm through a two-hop game-task defer.**

## 0.31.0

### Free-standing alignment: each real player owns their OStim alignment

The same human participant is represented by two different Skyrim actors in multiplayer:

```text
Player2 client: real Elir PlayerCharacter
Player1 client: dynamic STR proxy representing Elir
```

0.31.0 adds a reliable/ordered STRPM alignment channel:

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

Authority is symmetric. This path performs no skeleton write and uses OStim's public `SetActorAlignment()` once per node.

### OCum equip-object mesh detection

`ocumvagmesh` and `ocumanmesh` are OStim equip-object types. The optional integration uses:

```text
OActor.IsObjectEquipped(Target, type)
OR
Target.IsEquipped(the corresponding OCum armor)
```

0.31.5 makes that query recurring for the duration of a player scene instead of depending exclusively on the OCum custom event.

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

## Free-standing world-position ownership

For ordinary shared free-standing scenes:

- OStim owns the real local PlayerCharacter reference, rendered root and animation;
- STR remains the source of incoming remote-player movement samples;
- OStim Together may correct only the **remote proxy's logical `TESObjectREFR::data.location`** to the common scene center;
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

Direct player-targeted starts are gated before OStim receives the scene-start input. The mandatory Core `OSKSE.pex` patch also suspends OStim's UIExtensions Add Actor callback when a remote STR player is selected. OStim Together's `OSKSE.pex` must win its conflict against OStim's original file.

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
- read-only local/proxy rendered-root diagnostics;
- equipment/outfit protection with OCum.esp runtime armor exemption;
- native OCum worn-armor fallback;
- RaceMenu/SKEE overlay rebuild support;
- generic addon state reapplication;
- optional OCum Ascended live OStim equip-object/overlay polling.

## Compatibility

Validated OStim runtime layouts:

- OStim 7.4c — `7.4.0.3`;
- OStim 7.5b — `7.5.0.2`.

The free-scene phase/alignment specialization requires Threads API v3 for exact furniture exclusion and therefore targets the validated OStim 7.5b path. Furniture/wall handling remains separate.

The required `OSKSE.pex` compatibility patch is based on the OStim 7.5b `OSKSE.psc` interface. Revalidate it when updating OStim.

## Build

0.31.5 changes the optional OCum Papyrus integration. Recompile it first:

```powershell
.\optional\OCumIntegration\compile-ocum-integration.ps1 `
  -SkyrimDir "C:\Games\Steam\steamapps\common\Skyrim Special Edition"
```

The helper outputs:

```text
optional\OCumIntegration\package\Data\Scripts\OStimTogetherOCum.pex
```

Then build the FOMOD:

```powershell
$env:VCPKG_ROOT="C:\dev\vcpkg"
.\build-fomod.ps1
```

Expected output:

```text
dist\OStimTogether-v0.31.5-FOMOD.zip
```

FOMOD layout:

- `00 Core` — required DLL/INI plus mandatory Add Actor gate scripts;
- `10 OCum Ascended` — optional OCum integration with the freshly compiled `OStimTogetherOCum.pex`.
