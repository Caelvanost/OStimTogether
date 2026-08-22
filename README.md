# OStim Together

Current development version: **0.27.4**.

The root `VERSION` file is the single source of truth for CMake, DLL startup logs and archive names. Small fixes increment patch; larger feature/architecture work increments minor and resets patch.

OStim Together synchronizes OStim Standalone scenes between Skyrim Together Reborn players. The `strpm` branch uses **STRPluginMessagingAPI only**; there is no UDP fallback.

## 0.27.4

### Free-standing scenes: never warp the STR proxy animation root

The 0.27.3 runtime logs finally isolated the remaining no-furniture alignment failure. Actor order and the shared scene center were consistent between Player1 and Player2, and the mirror was no longer recomputing a competing center. The remaining correction was the legacy STR proxy pose guard itself.

On a free-standing START, the guard logged the remote proxy moving from its network/reference position to the authoritative OStim origin and then reported:

```text
rootBefore=(remote STR position)
target=(OStim origin)
rootAfter=(OStim origin)
```

`rootAfter` became identical to the reference because `ForceSTRProxyPose()` calls `Update3DPosition(true)`. That is appropriate for a fixed furniture/wall anchor, but it is destructive for free-standing paired animations: OStim has already started the actor animation, whose rendered root/skeleton must be allowed to carry the animation's own relative displacement. Warping the 3D back onto the reference origin resets that displacement and produces a visually misaligned participant even though the world-space reference coordinates look correct.

0.27.4 therefore removes all full-pose correction from ordinary no-furniture/non-wall scenes:

```text
OStim ChangeNode / START
→ native OStim lockAtPosition()
→ OStim starts the paired animation
→ OStim Together immediately disables the full STR proxy pose guard
→ short delay
→ StopTranslation() only
→ STR owns the remote reference
→ OStim/animation owns the rendered root
```

For free scenes OStim Together now performs **none** of the following on an STR proxy:

- `SetPosition()`;
- direct reference-position writes;
- `Update3DPosition(true)`;
- convergence hard-snaps;
- periodic pose corrections;
- mirror center reconstruction.

The delayed release logs both the reference and rendered root before and after `StopTranslation()` without modifying either transform:

```text
OSTNET FREE SCENE ROOT NATIVE ... poseGuard=0 setPosition=0 update3D=0
OSTNET FREE SCENE ROOT RELEASE ... rootTouched=0
OSTNET FREE SCENE ROOT OWNERSHIP ... referenceOwner=STR animationRootOwner=OStim poseGuard=0 continuousWrites=0
```

Furniture and wall scenes are intentionally unchanged and retain the established anchored pose guards.

## 0.27.3

### Mirror free-scene alignment: preserve the authoritative owner center

0.27.2 proved that the remaining no-furniture instability was not caused by the remote player moving during consent. Player1/owner convergence reached the OStim pose normally, while Player2/mirror timed out on every free START/NODE with a nearly constant 20–40 unit error even when Elir stood still.

The decisive log pair was:

```text
OSTNET STR PROXY POSE GUARD ... mirror=1 ... maxRefAfterDist2=0
OSTNET FREE SCENE CONVERGENCE ... mirror thread ... maxDist=23..38 ... timeout=1
```

The authoritative pose guard had already placed the remote proxy exactly on the owner's START pose, but the 0.27.2 convergence barrier independently called `TryComputeSceneCenter()` on the mirror. That function reconstructs a center from the local PlayerCharacter's **current animated position**. Once the animation/root motion had started, this reconstructed mirror center no longer represented the immutable owner thread origin. The convergence barrier therefore generated a second target while the authoritative pose guard continued enforcing the owner's target. The two systems fought the same proxy.

0.27.3 separated the owner and mirror paths, preserving the owner's initial center on mirrors. 0.27.4 supersedes the remaining free-scene pose-guard stage because the guard itself was shown to overwrite the rendered animation root.

Furniture and wall scenes remain unchanged.

## 0.27.2

### Free-standing startup convergence barrier

0.27.1 removed the long-lived free-scene position corrections, but runtime logs showed one remaining startup race. During consent and OStim setup the selected remote player can continue moving. At the real OStim START the remote STR proxy may therefore still be tens or hundreds of Skyrim units from the final scene center. OStim correctly starts `lockAtPosition()/TranslateTo()` to bring that proxy into the paired-animation origin, but OStim Together's legacy START/NODE/SPEED release path could call `StopTranslation()` only a few milliseconds later.

0.27.2 introduced a convergence barrier. Later tests showed that treating the proxy reference position as the authoritative visual alignment criterion was still incorrect for free-standing root-motion animations, so the convergence writes are removed in 0.27.4.

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

For OStim 7.5b no-furniture, non-wall scenes, 0.27.1 removed those competing continuous position owners after the initial scene construction. 0.27.4 completes that direction by removing the remaining startup/node full 3D proxy warp as well.

Furniture and wall scenes retain the established anchored alignment paths.

Free-standing `NODE` packets deliberately omit `poses=`. The node ID is still synchronized, but START/NODE poses are no longer treated as permanent world coordinates after a free scene has begun.

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

Shared scene control is **multi-master**. No scene participant has to obtain command approval from the player who originally started the scene.

The shared protocol currently covers:

```text
CONTROL_NODE
CONTROL_SPEED
CONTROL_STOP
```

Player1, Player2, and additional accepted participants can all control the same scene with equal control rights. The original starter is not privileged after consent and scene creation. For concurrent commands, the owner route is used only as a deterministic relay/sequencer so every client converges on one ordering.

Actor alignment-editor offsets and arbitrary OStim Scene Options that invoke local Papyrus functions are **not yet declared synchronized state**.

## 0.26.2

### Free-standing scene alignment

0.26.2 was the first attempt to stop the long-lived STR proxy pose guard from fighting free-standing animation root motion. It released the proxy to STR after initial stabilization. The later 0.27.x tests progressively removed the remaining continuous reference corrections, mirror NPC alignment and finally the full rendered-root warp in 0.27.4.

The no-furniture specialization applies to OStim 7.5b / Threads ABI v3, where `getFurnitureObject()` can prove the scene has no furniture. Furniture and wall scenes keep their dedicated handling.

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

As of 0.26.1, `OSKSE.pex` and `OStimTogetherNative.pex` are always included in **00 Core**. OStim Together's `OSKSE.pex` must win its file conflict against OStim's original `OSKSE.pex`.

### Add Actor acceptance fix

0.26.0 could briefly emit an `actor-missing` cancel after acceptance of the temporary Add Actor gate. 0.26.1 keeps a local-only completion guard so the pseudo-session cannot enter the real owner-scene start path.

### OCum state snapshot before Accept

The remote client sends its current OCum state before `INVITE_RESPONSE` on Accept:

- RaceMenu overlay chunks whose texture contains `CumOverlays`;
- `ocumvagmesh` equipped state;
- `ocumanmesh` equipped state.

Because STRPM transport is reliable/ordered, the initiating client receives the appearance state before it is allowed to start the synchronized scene.

## Other synchronization features

- targeted STRPM consent and scene traffic;
- exact OStim 7.5 furniture synchronization through Threads ABI v3;
- OStim 7.4c furniture fallback;
- Wall-scene startup handling;
- shared native OStim node/speed/stop control for every accepted player participant;
- root-motion-native free-scene proxy handling for OStim 7.5b;
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

First compile the mandatory Add Actor Papyrus compatibility patch when its Papyrus sources change:

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

For 0.27.4 no Papyrus source changed, so existing compiled PEX files can be reused.

Then build the FOMOD:

```powershell
$env:VCPKG_ROOT="C:\dev\vcpkg"
.\build-fomod.ps1
```

Expected output:

```text
dist\OStimTogether-v0.27.4-FOMOD.zip
```

FOMOD layout:

- `00 Core` — required; includes DLL, INI, `OSKSE.pex` and `OStimTogetherNative.pex`;
- `10 OCum Ascended` — optional live OCum integration.
