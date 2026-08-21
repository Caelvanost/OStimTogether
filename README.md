# OStim Together

Current development version: **0.26.0**.

The root `VERSION` file is the single source of truth for CMake, DLL startup logs and archive names. Small fixes increment patch; larger feature/architecture work increments minor and resets patch.

OStim Together synchronizes OStim Standalone scenes between Skyrim Together Reborn players. The `strpm` branch uses **STRPluginMessagingAPI only**; there is no UDP fallback.

## 0.26.0

### Direct player start: consent before any OStim UI

The 0.25.0 input gate remains unchanged:

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

### Add Actor: pause at the selected remote player

0.26.0 adds a UIExtensions compatibility component under `compat/OStimUIConsent/`.

OStim's `Game::showMessageBox()` routes multi-option dialogs through `OSKSE.UIExtMessageBox()` when UIExtensions is installed. The compatibility patch replaces only that Papyrus bridge and keeps the OStim setup coroutine suspended after a remote-player entry is selected:

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

The patch is packaged as `20 OStim Add Actor Consent Gate`. Its `OSKSE.pex` must win the file conflict against OStim's original `OSKSE.pex`. The guarded thread-preflight path remains as a safety fallback when this component is absent.

### OCum state snapshot before Accept

The 0.25.0 logs showed RaceMenu proxy overlays rebuilding correctly but the generic addon cache remaining empty (`overlayChunks=0 objects=0`) when the remote player's OCum state predated the scene.

0.26.0 sends the real local player's current OCum state **before** `INVITE_RESPONSE` on Accept:

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

## Shared controls

The initiator remains authoritative. Accepted participants can navigate, change speed and stop the scene. Remote requests use `CONTROL_NODE`, `CONTROL_SPEED` and `CONTROL_STOP`; the resulting authoritative state is fanned back to every participant with echo suppression.

Speed reads are deferred outside OStim's SPEED callback to avoid the reentrant deadlock fixed in 0.23.2.

## Other synchronization features

- targeted STRPM consent and scene traffic;
- exact OStim 7.5 furniture synchronization through Threads ABI v3;
- OStim 7.4c furniture fallback;
- Wall-scene startup handling;
- STR proxy pose stabilization;
- equipment/outfit protection and residual apparel restoration;
- RaceMenu/SKEE overlay rebuild support;
- generic addon state reapplication;
- optional live OCum Ascended integration.

## Compatibility

Validated OStim runtime layouts:

- OStim 7.4c — `7.4.0.3`;
- OStim 7.5b — `7.5.0.2`.

The `OSKSE.pex` compatibility patch is based on the OStim 7.5b `OSKSE.psc` interface. Revalidate it when updating OStim.

## Build

First compile the Add Actor Papyrus compatibility patch. Point `-OStimSourceDir` at OStim's `data\Scripts\Source` directory and, if required, `-UIExtensionsSourceDir` at UIExtensions Papyrus sources:

```powershell
.\compat\OStimUIConsent\compile-ui-consent.ps1 `
  -SkyrimDir "C:\Games\Steam\steamapps\common\Skyrim Special Edition" `
  -OStimSourceDir "<path-to-OStim-source>" `
  -UIExtensionsSourceDir "<path-to-UIExtensions-source>"
```

This must produce:

```text
compat\OStimUIConsent\package\Data\Scripts\OSKSE.pex
compat\OStimUIConsent\package\Data\Scripts\OStimTogetherNative.pex
```

Then build the FOMOD:

```powershell
$env:VCPKG_ROOT="C:\dev\vcpkg"
.\build-fomod.ps1
```

Expected output:

```text
dist\OStimTogether-v0.26.0-FOMOD.zip
```

FOMOD layout:

- `00 Core` — required;
- `10 OCum Ascended` — optional live OCum integration;
- `20 OStim Add Actor Consent Gate` — recommended with UIExtensions, required for the exact Add Actor pause behavior.
