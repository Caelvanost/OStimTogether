# OStim Together

Current development version: **0.27.0**.

The root `VERSION` file is the single source of truth for CMake, DLL startup logs and archive names. Small fixes increment patch; larger feature/architecture work increments minor and resets patch.

OStim Together synchronizes OStim Standalone scenes between Skyrim Together Reborn players. The `strpm` branch uses **STRPluginMessagingAPI only**; there is no UDP fallback.

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

```text
participant selects an OStim node
→ that participant's local OStim thread changes immediately
→ CONTROL_NODE is emitted automatically
→ the existing owner route acts only as a transport relay / ordering point
→ membership guard checks only that the sender belongs to the active scene
→ NavigateToScene is applied automatically
→ resulting scene state is fanned to every participant
```

There is no owner veto, confirmation dialog, permission decision, or gameplay-level validation step. The only guard retained is the active-session membership check needed to reject control traffic from clients whose characters are not participants in that scene.

The same model is used for speed and stop:

```text
CONTROL_SPEED
CONTROL_STOP
```

This means Player1, Player2, and additional accepted participants can all control the same scene with equal control rights. The original starter is not privileged after consent and scene creation.

For concurrent commands, the owner route is used only as a deterministic **relay/sequencer** so every client converges on one ordering. Commands are not accepted or rejected according to which participant issued them; among active participants, the transport arrival order determines the resulting shared state.

Current 0.27.0 shared-control scope:

- native OStim scene navigation from the current node;
- OStim search/navigation results, because they resolve to normal node changes;
- speed up/down;
- stop/end scene through the existing synchronized stop path.

Actor alignment-editor offsets and arbitrary OStim Scene Options that invoke local Papyrus functions are **not yet declared synchronized state**. They remain outside the 0.27.0 shared-control contract until OStim Together has explicit protocol messages for those values.

## 0.26.2

### Free-standing scene alignment

Logs from 0.26.1 showed that ordinary scenes with no furniture could gradually separate remote-player proxies from local actors even though furniture scenes stayed aligned.

The cause was the long-lived STR proxy pose guard. OStim Together deliberately stopped continuously pinning the real local PlayerCharacter because that fought animation root motion, but the dynamic STR proxy was still forced back to its computed scene pose every rendered frame. In standing/free animation packs, the local player and NPCs could therefore follow animation root motion while the remote proxy remained pinned near the original scene center.

0.26.2 keeps the existing initial stabilization, then releases only that continuous proxy position ownership for ordinary free-standing scenes:

```text
OStim START
→ normal initial OStim alignment
→ existing short STR-proxy settle window (~200 ms)
→ 250 ms: disable continuous proxy pose guard
→ StopTranslation on the STR proxy
→ STR + animation root motion own the proxy for the rest of the scene
```

This correction is deliberately narrow:

- OStim 7.5b / Threads ABI v3 only, where `getFurnitureObject()` can prove the scene has no furniture;
- scenes with an actual furniture reference are unchanged;
- wall scenes are unchanged and keep their dedicated stabilization path;
- OStim 7.4c keeps the previous behavior rather than relying on a furniture heuristic.

Expected log for an affected free-standing scene:

```text
OSTNET FREE SCENE ALIGN armed thread=... furniture=none action=release-proxy-after-initial-align
OSTNET STR PROXY POSE GUARD disabled thread=... reason=free-scene-no-furniture owner=STR rootMotion=enabled
OSTNET FREE SCENE PROXY RELEASE thread=... proxies=1 action=stop-translation owner=STR rootMotion=enabled
```

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

### 0.26.1 Add Actor acceptance fix

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
- STR proxy pose stabilization, with root-motion release for ordinary 7.5b free scenes;
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

First compile the mandatory Add Actor Papyrus compatibility patch. UIExtensions and the small OStim dependencies required only for compilation are stubbed inside the repository, so no external UIExtensions source tree is required:

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

Then build the FOMOD:

```powershell
$env:VCPKG_ROOT="C:\dev\vcpkg"
.\build-fomod.ps1
```

Expected output:

```text
dist\OStimTogether-v0.27.0-FOMOD.zip
```

FOMOD layout:

- `00 Core` — required; includes DLL, INI, `OSKSE.pex` and `OStimTogetherNative.pex`;
- `10 OCum Ascended` — optional live OCum integration.
