# OStim Together

Current development version: **0.31.0**.

The root `VERSION` file is the single source of truth for CMake, DLL startup logs and archive names. Small fixes increment patch; larger feature/architecture work increments minor and resets patch.

OStim Together synchronizes OStim Standalone scenes between Skyrim Together Reborn players. The `strpm` branch uses **STRPluginMessagingAPI only**; there is no UDP fallback.

## 0.31.0

### Free-standing alignment: each real player owns their OStim alignment

The 0.30.5 runtime test established an important boundary: both clients now converged to the **same** free-standing arrangement, but that common arrangement could still be wrong relative to the paired animation. That rules out ordinary STR position divergence as the remaining primary fault.

The same human participant is represented by two different Skyrim actors in multiplayer:

```text
Player2 client: real Elir PlayerCharacter
Player1 client: dynamic STR proxy representing Elir
```

OStim derives an alignment key from actor characteristics and resolves `ActorAlignmentData` from that key. A dynamic STR proxy is not guaranteed to resolve the same alignment key/cache entry as the real PlayerCharacter it represents. Re-applying each client's independently-derived proxy alignment can therefore make both clients network-consistent while still using the wrong OStim offset for the paired animation.

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
→ existing clock-calibrated PHASE_COMMIT / SetSpeed replay
```

Authority is symmetric:

- Kahel's real PlayerCharacter is authoritative only for Kahel's OStim alignment;
- Elir's real PlayerCharacter is authoritative only for Elir's OStim alignment;
- additional participants follow the same rule.

The receiver applies the state only if the sender's resolved STR proxy is actually present in the active local OStim player thread and the node IDs match. Early packets get a few bounded retries while the mirror catches up.

This path performs **no** direct skeleton write, no `SetPosition`, no `Update3DPosition`, and no continuous pose correction. It uses OStim's public `SetActorAlignment()` once per node. Furniture and wall scenes are excluded and keep their established anchored handling.

Expected logs:

```text
OSTNET ALIGN SYNC READY ... authority=real-local-player ...
OSTNET ALIGN SYNC TRANSPORT READY channel=ostimtogether.align ...
OSTNET ALIGN SELF TX ... offset=(...) scale=... rotation=... sosBend=...
OSTNET ALIGN APPLY ... proxy=... offset=(...) source=remote-real-player ...
```

These logs are also diagnostic: if both real players publish only zero/default alignment yet the paired animation is still visibly offset, the remaining problem is not OStim's alignment cache and the next investigation can focus on the animation graph/phase without touching world or skeleton transforms.

### OCum equip-object mesh detection

The same runtime test showed the OCum transport/application path was still alive, but Player2 repeatedly published:

```text
vagMesh=0 analMesh=0
```

and Player1 correctly applied those false states. The failure was therefore local state detection, not STRPM delivery.

`ocumvagmesh` and `ocumanmesh` are OStim equip-object types. The optional OCum integration now reads both representations:

```text
OActor.IsObjectEquipped(Target, type)
OR
Target.IsEquipped(the corresponding OCum armor)
```

Either source being true publishes the mesh as equipped. This preserves the armor fallback while restoring OStim's own equip-object state as a source of truth.

Because this is a Papyrus-source change, **`OStimTogetherOCum.pex` must be recompiled for 0.31.0**:

```powershell
.\optional\OCumIntegration\compile-ocum-integration.ps1 `
  -SkyrimDir "C:\Games\Steam\steamapps\common\Skyrim Special Edition"
```

The compile helper uses the local compile-only `OActor.psc` stub and outputs only:

```text
optional\OCumIntegration\package\Data\Scripts\OStimTogetherOCum.pex
```

Do not package or compile the stub as `OActor.pex`; OStim provides the real runtime implementation.

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
→ every client replays through OStim's native SetSpeed(currentSpeed) path
```

0.30.5 re-applies OStim actor alignment before the synchronized replay to reproduce the native `ChangeNode` ordering (`alignActors -> SetSpeed -> playAnimation`). In 0.31.0 the remote-proxy alignment cache has first been updated from the corresponding real participant through `ostimtogether.align`.

A delayed native `StopTranslation` clears the transient OStim `TranslateTo` on dynamic STR proxies. The current implementation uses the TESObjectREFR Papyrus native relocation; `RE::Actor` has no CommonLib `StopTranslation()` member.

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

Ordinary free-standing scenes do not use a continuous OStim Together proxy pose guard. Skyrim Together owns the dynamic remote `TESObjectREFR` position; OStim handles its native scene alignment and animation work. OStim Together does not continuously pin free-scene proxies to START/NODE world coordinates.

Furniture and wall scenes retain their dedicated anchored paths.

## Shared scene control

Every accepted player participant gets OStim's native SceneMenu. Scene control is multi-master and synchronizes:

```text
CONTROL_NODE
CONTROL_SPEED
CONTROL_STOP
```

Player1, Player2 and additional accepted participants have equal control rights. The original initiator is only a deterministic relay/sequencing point; there is **no owner approval or veto**.

Shared SPEED is idempotent so an already-current incoming speed does not trigger another `SetSpeed()` replay and feedback loop.

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
- proxy-only auxiliary OStim thread cleanup;
- equipment/outfit protection and residual apparel restoration;
- RaceMenu/SKEE overlay rebuild support;
- generic addon state reapplication;
- optional live OCum Ascended overlay and equip-object integration.

## Compatibility

Validated OStim runtime layouts:

- OStim 7.4c — `7.4.0.3`;
- OStim 7.5b — `7.5.0.2`.

The free-scene phase/alignment specialization requires Threads API v3 for exact furniture exclusion and therefore targets the validated OStim 7.5b path. Furniture/wall handling remains separate.

The required `OSKSE.pex` compatibility patch is based on the OStim 7.5b `OSKSE.psc` interface. Revalidate it when updating OStim.

## Build

The mandatory Add Actor Papyrus patch only needs recompilation when its sources change. **It did not change in 0.31.0.**

The optional OCum integration **did change** and must be recompiled:

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
dist\OStimTogether-v0.31.0-FOMOD.zip
```

FOMOD layout:

- `00 Core` — required DLL/INI plus mandatory Add Actor gate scripts;
- `10 OCum Ascended` — optional OCum integration with the newly compiled `OStimTogetherOCum.pex`.
