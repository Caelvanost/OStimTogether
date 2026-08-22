# OStim Together

Current development version: **0.27.2**.

The root `VERSION` file is the single source of truth for CMake, DLL startup logs and archive names. Small fixes increment patch; larger feature/architecture work increments minor and resets patch.

OStim Together synchronizes OStim Standalone scenes between Skyrim Together Reborn players. The `strpm` branch uses **STRPluginMessagingAPI only**; there is no UDP fallback.

## 0.27.2

### Free-standing startup convergence barrier

0.27.1 removed the long-lived free-scene position corrections, but runtime logs showed one remaining startup race. During consent and OStim setup the selected remote player can continue moving. At the real OStim START the remote STR proxy may therefore still be tens or hundreds of Skyrim units from the final scene center. OStim correctly starts `lockAtPosition()/TranslateTo()` to bring that proxy into the paired-animation origin, but OStim Together's legacy START/NODE/SPEED release path could call `StopTranslation()` only a few milliseconds later.

That made the selected player's movement during the unpaused UI visible as a persistent alignment error. The movement itself is valid; canceling OStim's initial convergence was the bug.

0.27.2 adds a short convergence barrier for OStim 7.5b no-furniture, non-wall scenes:

```text
START / free NODE
→ OStim starts native lockAtPosition / TranslateTo
→ OStim Together samples the current OStim target every 50 ms
→ while proxy distance > 4 units:
     restore native TranslateTo toward the current OStim pose
→ require 3 consecutive samples within 4 units
→ disable temporary STR-proxy pose guard
→ StopTranslation once
→ READY: no further continuous position corrections
```

The barrier is capped at 2.5 seconds. If convergence never stabilizes, 0.27.2 performs one final hard snap to the current OStim pose and immediately releases the proxy rather than leaving an indefinite STR/OStim position fight.

Expected logs:

```text
OSTNET FREE SCENE ALIGN READY ... mode=convergence-barrier ...
OSTNET FREE SCENE ALIGN ARM ... action=wait-for-proxy-convergence
OSTNET FREE SCENE CONVERGENCE ... maxDist=... stable=.../3
OSTNET FREE SCENE ALIGN READY ... timeout=0 ... action=release-to-str-animation
```

If the last-resort path is used:

```text
OSTNET FREE SCENE ALIGN READY ... timeout=1 ...
```

Furniture and wall scenes are unchanged and retain their established anchored guards.

### Idempotent shared SPEED

OStim's `SetSpeed()` replays the current animation and emits a speed event even when the requested speed is already active. In a multi-master scene, replaying an identical incoming `SPEED` could therefore create participant → relay → participant feedback and repeated animation restarts.

0.27.2 makes shared speed application idempotent on both sides:

- a mirror ignores an authoritative `SPEED` that already matches its current speed;
- the relay/owner does not call `SetSpeed()` when a participant requests the already-current speed;
- in that no-op relay case the current speed is still fanned out explicitly so additional participants converge without replaying the animation.

Expected logs:

```text
OSTNET MIRROR SPEED NOOP ... reason=already-current
OSTNET COOP CONTROL SPEED NOOP ... reason=already-current fanout=...
```

There is still no owner approval or veto for speed changes; this is only duplicate-state suppression.

## 0.27.1

### Free-standing scenes: native motion ownership

Runtime logs from 0.27.0 showed that Skyrim Together and OStim were already moving participating players consistently, while OStim Together was continuously restoring obsolete START/NODE world coordinates. The remote STR proxy and the real remote PlayerCharacter could therefore agree with each other, then be pulled back toward the old scene origin by OStim Together. The mirror also re-applied `SetActorAlignment()` to local NPCs every refresh while SELF and player proxies were following animation/root motion.

For OStim 7.5b no-furniture, non-wall scenes, 0.27.1 removes those competing position owners after the initial scene construction:

```text
START common origin
→ create mirror at the same initial center
→ native OStim initial alignment
→ short settle window
→ disable OStim Together full STR-proxy pose guard
→ StopTranslation once on the STR proxy
→ no replacement reference guard
→ no periodic SetActorAlignment on mirror NPCs
→ no authoritative SELF world-pose reassert on free NODE packets
```

After the settle:

```text
remote STR proxy  -> STR network movement + animation
local SELF        -> OStim + animation/root motion
local NPCs        -> OStim + animation/root motion
OStim Together    -> node/speed/stop synchronization only
```

Furniture and wall scenes retain the established anchored alignment paths.

Free-standing `NODE` packets deliberately omit `poses=`. The node ID is still synchronized, but START/NODE poses are no longer treated as permanent world coordinates after a free scene has begun.

Expected logs:

```text
OSTNET FREE SCENE ALIGN READY ... mode=noFurniture-native-ownership continuousCorrections=0
OSTNET RESOLVE START ... continuousMirrorAlign=0
OSTNET|v1|NODE|... poseMode=free-native
OSTNET FREE SCENE NATIVE OWNERSHIP ... continuousCorrections=0 proxyOwner=STR actorsOwner=OStim-animation
```

### Shared-control routing fixes

0.27.0 exposed the native OStim SceneMenu on every involved player's mirror, but the first multiplayer tests revealed two independent routing bugs.

A stale consent timeout could fire after an older scene had already ended. Because OStim commonly reuses `thread=0`, the old session could erase `_activeOwnerByThread[0]` after a newer scene had already claimed that thread ID. 0.27.1 now times out only sessions that never obtained an active thread, and cancel cleanup removes a thread route only when it still belongs to the same session.

`CONTROL_NODE` also previously used `NavigateToScene()`. OStim's implementation returns `OK` when the task is queued, but `Thread::Navigate()` only succeeds when the supplied ID is a valid navigation entry from the owner's current node. If Player2 and Player1 were at different points in a transition chain, Player2 could advance locally while Player1 stayed on the previous node.

0.27.1 applies participant node commands with `NavigateToSearchResult()`, which resolves the exact node ID and queues `Thread::ChangeNode()` directly:

```text
Player2 selects/reaches node X
→ CONTROL_NODE(thread, X)
→ membership guard only
→ owner relay applies exact ChangeNode(X)
→ resulting NODE is fanned to all participants
```

There is still **no owner approval or veto**. The original starter remains only the relay/sequencing point for the shared session.

The mirror anti-echo state is also no longer armed when an incoming authoritative NODE or SPEED already matches the mirror's current state, preventing stale suppression from swallowing a later real participant command.

Expected control log:

```text
OSTNET COOP CONTROL NODE connection=... thread=... node=... result=0 apply=exact-change-node
```

## 0.27.0

### Shared native OStim scene control

Every client whose real local PlayerCharacter participates in a synchronized multiplayer scene now gets OStim's native SceneMenu instead of a read-only mirror.

OStim already computes the valid navigations from the current node, displays the current speed, supports speed up/down, and routes search results back into normal node navigation. OStim Together therefore does not maintain a second scene database or duplicate the OStim UI.

Multiplayer threads are detected by the presence of both the real local player and at least one dynamic STR remote-player proxy. On those threads OStim Together calls the public OStim `OPlayerThread.SetPlayerControl(true)` bridge after the scene thread is fully registered. This removes `NO_PLAYER_CONTROL` and refreshes the native SceneMenu.

Expected startup log:

```text
OSTNET SHARED CONTROL READY ... routing=multi-master ownerRole=relay-sequencer commandApproval=none ...
```

Expected per-scene logs:

```text
OSTNET SHARED CONTROL ARM thread=... action=enable-native-player-control
OSTNET SHARED CONTROL ENABLE thread=... nativeMenu=1 participantCommands=1 multiMaster=1 commandApproval=none
```

Shared scene control is **multi-master**. No scene participant has to obtain command approval from the player who originally started the scene.

The same model is used for speed and stop:

```text
CONTROL_NODE
CONTROL_SPEED
CONTROL_STOP
```

This means Player1, Player2, and additional accepted participants can all control the same scene with equal control rights. The original starter is not privileged after consent and scene creation.

For concurrent commands, the owner route is used only as a deterministic relay/sequencer so every client converges on one ordering. Commands are not accepted or rejected according to which participant issued them; among active participants, transport arrival order determines the resulting shared state.

Current shared-control scope:

- native OStim scene navigation from the current node;
- OStim search/navigation results;
- speed up/down;
- stop/end scene through the synchronized stop path.

Actor alignment-editor offsets and arbitrary OStim Scene Options that invoke local Papyrus functions are **not yet declared synchronized state**. They remain outside the shared-control contract until OStim Together has explicit protocol messages for those values.

## 0.26.2

### Free-standing scene alignment

0.26.2 was the first attempt to stop the long-lived STR proxy pose guard from fighting free-standing animation root motion. It released the proxy to STR after the initial stabilization window. Runtime tests in 0.27.0 later showed that a replacement reference guard and periodic mirror NPC alignment still competed with valid OStim/STR motion; those remaining continuous corrections are removed in 0.27.1.

The 0.26.2 scope remains important historically:

- OStim 7.5b / Threads ABI v3 only, where `getFurnitureObject()` can prove the scene has no furniture;
- scenes with an actual furniture reference are unchanged;
- wall scenes are unchanged and keep their dedicated stabilization path;
- OStim 7.4c keeps the previous behavior rather than relying on a furniture heuristic.

## 0.26.1

### Direct player start: consent before any OStim UI

The pre-UI input gate remains unchanged:

```text
Aim at remote STR player proxy
→ OStim scene-start key
→ OStim Together consumes the key before OStim sees it
→ "Waiting for consent"
→ remote SafeMessageBox: Accept / Decline
→ Accept
→ OStim resumes its normal Furniture / Add Actor / role / fade pipeline
```

No Furniture menu, Add Actor menu, fade, undressing or OStim thread should appear before the remote player accepts.

### Add Actor consent is mandatory Core functionality

OStim's `Game::showMessageBox()` routes multi-option dialogs through `OSKSE.UIExtMessageBox()` when UIExtensions is installed. OStim Together patches only that Papyrus bridge and keeps the OStim setup coroutine suspended after a remote-player entry is selected:

```text
Player + NPC setup
→ Furniture
→ Add Actor
→ select remote player proxy
→ OStimTogetherNative.BeginAddActorConsent(name)
→ "Waiting for consent"
→ remote SafeMessageBox
→ while pending: OSKSE.UIExtMessageBox does NOT return the selected index
→ Accept
→ next OStim thread is armed as already consented
→ UIExtMessageBox returns the selected actor index
→ OStim continues to role selection / fade / scene
```

If consent is declined or times out, the same Add Actor list is reopened instead of allowing OStim to continue with that remote actor.

As of 0.26.1, `OSKSE.pex` and `OStimTogetherNative.pex` are always included in **00 Core**. The gate is no longer an optional FOMOD component because omitting it would restore the unsafe late-preflight behavior for NPC → Add Actor → remote-player scenes.

OStim Together's `OSKSE.pex` must win its file conflict against OStim's original `OSKSE.pex`.

### Add Actor acceptance fix

0.26.0 used the generic accepted-session path for the temporary Add Actor consent pseudo-session. That pseudo-session intentionally had no real OStim actor list, so after the remote player clicked Accept it briefly produced:

```text
INVITE_CANCEL|session=...|reason=actor-missing
```

The real scene still resumed correctly, but the remote client displayed a misleading "scene invitation canceled" notification.

0.26.1 keeps a local-only completion guard in the temporary consent session. The generic handler records the real participant's acceptance but cannot enter `StartApprovedOwnerSession()` for the pseudo-session. On the next Papyrus poll, the guard is removed and the accepted participant set is normalized before the real OStim thread is armed. No `actor-missing` cancel packet should be emitted after Accept.

Expected owner-side sequence:

```text
OSTNET ADD-ACTOR GATE BEGIN ... uiPaused=1
OSTNET COOP INVITE RESPONSE RX ... accepted=1 start=0
OSTNET ADD-ACTOR GATE ACCEPT ... action=arm-next-thread-resume-ui
```

There should be no `INVITE_CANCEL ... reason=actor-missing` between those lines.

### OCum state snapshot before Accept

The 0.25.x logs showed RaceMenu proxy overlays rebuilding correctly but the generic addon cache remaining empty (`overlayChunks=0 objects=0`) when the remote player's OCum state predated the scene.

0.26.x sends the real local player's current OCum state **before** `INVITE_RESPONSE` on Accept:

- all RaceMenu overlay chunks whose texture contains `CumOverlays`;
- `ocumvagmesh` equipped state;
- `ocumanmesh` equipped state.

Because these packets are queued before the consent response on STRPM reliable/ordered transport, the initiating client receives the appearance state before it is allowed to start the synchronized scene.

Expected receiver-side log on Accept:

```text
OSTNET OCUM SNAPSHOT TX reason=consent-accept ... overlayChunks=... vagMesh=... analMesh=...
OSTNET COOP INVITE RESPONSE TX ... accepted=1
```

Expected owner-side addon logs:

```text
OSTNET ADDON OVR RX ... channel=ocum ...
OSTNET ADDON OBJ RX ... type=ocumvagmesh ...
OSTNET ADDON OBJ RX ... type=ocumanmesh ...
```

## Other synchronization features

- targeted STRPM consent and scene traffic;
- exact OStim 7.5 furniture synchronization through Threads ABI v3;
- OStim 7.4c furniture fallback;
- Wall-scene startup handling;
- shared native OStim node/speed/stop control for every accepted player participant;
- convergence-gated free-scene startup for OStim 7.5b;
- equipment/outfit protection and residual apparel restoration;
- RaceMenu/SKEE overlay rebuild support;
- generic addon state reapplication;
- optional live OCum Ascended integration.

## Compatibility

Validated OStim runtime layouts:

- OStim 7.4c — `7.4.0.3`;
- OStim 7.5b — `7.5.0.2`.

The required `OSKSE.pex` compatibility patch is based on the OStim 7.5b `OSKSE.psc` interface. Revalidate it when updating OStim.

## Build

First compile the mandatory Add Actor Papyrus compatibility patch when its Papyrus sources change. UIExtensions and the small OStim dependencies required only for compilation are stubbed inside the repository, so no external UIExtensions source tree is required:

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

For 0.27.2 no Papyrus source changed, so existing compiled PEX files can be reused.

Then build the FOMOD:

```powershell
$env:VCPKG_ROOT="C:\dev\vcpkg"
.\build-fomod.ps1
```

Expected output:

```text
dist\OStimTogether-v0.27.2-FOMOD.zip
```

FOMOD layout:

- `00 Core` — required; includes DLL, INI, `OSKSE.pex` and `OStimTogetherNative.pex`;
- `10 OCum Ascended` — optional live OCum integration.
