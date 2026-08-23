# OStim Together

Current development version: **0.31.6**.

The root `VERSION` file is the single source of truth for CMake, DLL startup logs and archive names. Small fixes increment patch; larger feature/architecture work increments minor and resets patch.

OStim Together synchronizes OStim Standalone scenes between Skyrim Together Reborn players. The `strpm` branch uses **STRPluginMessagingAPI only**; there is no UDP fallback.

## 0.31.6

### Alignment: bounded correction of mirror SELF after animation startup

The 0.31.5 Kahel + Elir logs finally isolated the free-scene offset to the correct side of the network boundary.

For the same node, Player2's TRUE local Elir reported approximately:

```text
rootFromCenter=(+4.379,-21.961,0)
```

while Player1's STR proxy for Elir reported approximately:

```text
rootFromCenter=(+3.854,-21.572,-1)
```

The values are essentially the same displacement. STR therefore is not creating an additional offset on Player1: it is faithfully transporting the displacement already present on the true Player2 actor.

The first Player2 diagnostic already shows that displacement immediately after the mirror OStim START, before OStim Together's later PHASE REPLAY. The phase barrier is therefore not the original cause either. OStim's static alignment data and the tested scene JSON both request a zero positional offset; the visible drift is produced during the mirror actor's animation/root startup.

0.31.6 keeps the continuous owner/proxy rules unchanged but adds one bounded mirror-only correction path:

```text
remote mirror START/NODE
-> allow OStim startup + normal phase replay to settle
-> ~700 ms: if true local mirror player ref/root drift > 1 unit, move SELF once to authoritative center
-> ~1100 ms later: one backup check for late phase replay
-> no more SELF position writes until another node
```

This is intentionally not the old continuous self-position lock that caused oscillation. The locally-owned initiator player is never touched by this path. The remote STR proxy still receives only the existing logical `TESObjectREFR::data.location` center correction; its rendered root and skeleton are not directly written.

Expected mirror log:

```text
OSTNET MIRROR SELF ONESHOT ARMED ... maxWrites=2
OSTNET MIRROR SELF ONESHOT ... stage=1 ... wrote=1 ...
OSTNET MIRROR SELF ONESHOT ... stage=2 ... wrote=0|1 ...
```

If stage 1 succeeds, subsequent `FREE ROLE DIAG rootFromCenter` for Elir should collapse from roughly 22 units to near zero, and Player1's proxy should follow through STR.

### OCum: repair Papyrus registrations after loading an existing save

0.31.5 added an `ostim_start` / `ostim_end` live scene poll to `OStimTogetherOCum.psc`, but the 0.31.5 runtime test produced no `ADDON OVR TX` and no live `ADDON OBJ ... equipped=1` during the entire scene. The first valid Elir overlay capture happened only after STOP.

The reason is save persistence: replacing the PEX attached to the already-running Start Game Enabled integration quest does not rerun that quest script's `OnInit()`. Existing saves therefore retained the old registration set and never registered the new OStim start/end handlers.

0.31.6 fixes this from Core without requiring a new save or a console quest restart. After data load and, critically, again after `kPostLoadGame` restores Papyrus save state, Core locates:

```text
OSTogetherOCumIntegrationQuest
```

and directly invokes the attached script's public:

```text
OStimTogetherOCum.RegisterIntegration()
```

through the Papyrus VM.

Expected startup/load log when the optional integration is installed:

```text
OSTNET OCUM PAPYRUS REFRESH reason=data-loaded ... dispatched=1
OSTNET OCUM PAPYRUS REFRESH reason=post-load-game ... dispatched=1
```

Once registered, the existing 0.31.5 scene poll should produce live overlay/object traffic while OStim is still running.

No Papyrus source changed in 0.31.6. If `OStimTogetherOCum.pex` was already recompiled from the 0.31.5 source, it does not need to be recompiled again.

## 0.31.5

### OCum: poll OStim's live equip-object state for the whole scene

The 0.31.4 Player1 + Player2 runtime test established two important facts:

- Player2 could see the cum mesh during the scene even though the native F37/F3B inventory probe reported `vagMesh=0 analMesh=0` for the entire thread;
- by scene end Player2 had valid `CumOverlays` RaceMenu slots, but no live `ADDON OVR` packet had been emitted while the scene was running.

The backing armor is therefore not a reliable scene-time representation of an OStim equip object, and `ocum_applied_cum` is not reliable enough as the only synchronization trigger in a mirrored multiplayer scene.

The optional `OStimTogetherOCum` integration registers for OStim's standard thread-0 `ostim_start` and `ostim_end` events. While the scene is active it polls the TRUE local PlayerCharacter every 500 ms. Each poll:

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

### Alignment: local/proxy role diagnostics

0.31.5 added `FREE ROLE DIAG` and extended `PROXY ORIGIN LOCK` so the true local player's and remote proxy's reference/root positions and world scales can be compared directly. Those diagnostics produced the evidence used by the 0.31.6 bounded correction.

## 0.31.4

### Correct the STR proxy logical origin, never continuously lock the real player

In shared free-standing scenes only the STR remote-player proxy's logical `TESObjectREFR::data.location` is held on the common center. Its rendered 3D/root remains untouched by that continuous path. The real local PlayerCharacter is not continuously locked.

### Native OCum armor fallback

0.31.4 added a native watcher for the OCum F37/F3B backing armor and a local `QueueNiNodeUpdate` refresh when that armor is exposed as worn. Later testing established that OStim's own `OActor.IsObjectEquipped()` state is authoritative during a scene when inventory `IsWorn()` remains false.

## 0.31.3

### Remote participant uses the owner's START center

A remote mirror no longer derives another scene center from its own participant role. It reads the owner's authoritative START center.

### OCum equipment exempt from NPC anti-reequip locking

Armor records defined by `OCum.esp` are excluded from `EquipmentLock`, protecting OCum's helper and equip-object armors while ordinary NPC clothing remains governed by `SlotMask`.

## 0.31.2

### Player + NPC START freeze

Player + NPC scenes bypass the shared-origin system, and real multiplayer scenes arm their center logic through a two-hop game-task defer so OStim startup is not re-entered synchronously.

## 0.31.0

### Participant-authored OStim alignment

Each real player owns only their own OStim `ActorAlignmentData`, transported over the reliable/ordered:

```text
ostimtogether.align
```

The receiver applies those values to the corresponding STR proxy through OStim's public `SetActorAlignment()` path.

### OCum equip-object mesh detection

`ocumvagmesh` and `ocumanmesh` are OStim equip-object types. The optional integration uses:

```text
OActor.IsObjectEquipped(Target, type)
OR
Target.IsEquipped(the corresponding OCum armor)
```

## 0.30.5

### Clock-calibrated free-scene phase replay

Ordinary no-furniture/non-wall scenes use the reliable/ordered `ostimtogether.phase` channel with `PHASE_PREP / READY / COMMIT` and an OStim-native speed replay.

## 0.28.1 safety rule

The old experiment that copied `NPC Root [Root]` transforms between independently evaluated skeletons remains disabled.

Safety invariant:

```text
remote skeleton rotation writes = 0
remote skeleton scale writes = 0
recursive skeleton transform rebuild = 0
```

## Free-standing world-position ownership

For ordinary shared free-standing scenes:

- OStim owns the locally-owned real PlayerCharacter and its animation;
- STR supplies the remote player's movement sample;
- the remote STR proxy's logical `TESObjectREFR::data.location` may be held on the common center without directly writing proxy skeleton transforms;
- a **remote mirror's true local PlayerCharacter** may receive at most two post-animation center corrections per node in 0.31.6, only when its root actually drifted;
- there is no continuous true-player position lock.

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
- bounded mirror-self post-animation correction;
- equipment/outfit protection with OCum.esp runtime armor exemption;
- native OCum worn-armor fallback;
- RaceMenu/SKEE overlay rebuild support;
- generic addon state reapplication;
- optional OCum Ascended live OStim equip-object/overlay polling;
- automatic post-load repair of optional OCum Papyrus registrations.

## Compatibility

Validated OStim runtime layouts:

- OStim 7.4c — `7.4.0.3`;
- OStim 7.5b — `7.5.0.2`.

The free-scene phase/alignment specialization requires Threads API v3 for exact furniture exclusion and therefore targets the validated OStim 7.5b path. Furniture/wall handling remains separate.

The required `OSKSE.pex` compatibility patch is based on the OStim 7.5b `OSKSE.psc` interface. Revalidate it when updating OStim.

## Build

0.31.6 does **not** change `OStimTogetherOCum.psc`. If you already compiled the optional integration for 0.31.5, keep that PEX.

If it has not yet been compiled from the 0.31.5 source, run:

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
dist\OStimTogether-v0.31.6-FOMOD.zip
```

FOMOD layout:

- `00 Core` — required DLL/INI plus mandatory Add Actor gate scripts;
- `10 OCum Ascended` — optional OCum integration with `OStimTogetherOCum.pex`.
