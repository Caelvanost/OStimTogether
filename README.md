# OStim Together

Current development version: **0.25.0**.

The root `VERSION` file is the single source of truth for the project version. CMake, the DLL startup log, the Vortex archive name and the FOMOD archive name are derived from it.

Versioning rule used by this project:

- small fixes/adjustments increment the third number;
- larger feature or architecture changes increment the second number and reset the third.

OStim Together is an SKSE plugin that synchronizes OStim Standalone scenes between Skyrim Together Reborn players. The `strpm` branch uses **STRPluginMessagingAPI** as its only multiplayer transport.

## 0.25.0 — consent before any OStim pre-scene UI

For the normal direct-player flow, consent now happens **before OStim receives the scene-start input at all**.

When the local player presses OStim's scene-start key while aiming at a mapped Skyrim Together player proxy:

```text
scene-start key
    ↓
OStim Together input gate (runs before OStim's input sink)
    ↓
remote proxy -> STRPM ConnectionID
    ↓
original key event is consumed
    ↓
"Waiting for consent"
    ↓
targeted INVITE
    ↓
remote SafeMessageBox: Accept / Decline
    ↓
Accept
    ↓
OStim public StartScene API with actors only
    ↓
OStim resumes its normal pipeline:
Furniture selection
→ Add Actor
→ role selection
→ starting animation
→ fade
→ real thread creation
```

This means that for a direct Player1 -> Player2 start, **no Furniture menu, Add Actor menu, fade, undressing or OStim thread is created before Player2 accepts**.

The input gate discovers OStim's own configured `keySceneStart` through the public Thread ModAPI. At `kInputLoaded`, OStim Together places its input sink immediately before OStim's sink in `BSInputDeviceManager::sinks`; CommonLib dispatches those sinks in insertion order, so returning `kStop` reliably prevents OStim from seeing the gated key event.

Expected diagnostics:

```text
OSTNET INPUT GATE READY order=reordered-before-ostim ...
OSTNET COOP DIRECT GATE session=... target=... connection=... uiSuppressed=1 furnitureShown=0
OSTNET COOP INVITE TX session=... localSceneActive=0 source=pre-ui
```

After Player2 accepts:

```text
OSTNET COOP INVITE RESPONSE RX ... source=pre-ui
OSTNET COOP APPROVED START ... pipeline=normal-ostim-pre-scene-ui
```

Only then should OStim display its Furniture/Add Actor flow.

### Safe MessageBox

Remote consent continues to use the same native `MessageBoxData` strategy validated in Trade Together. `MessageDataFactoryManager` creates the message object and OStim Together changes only body text, buttons and callback, preserving factory-initialized fields so Skyrim Souls / Unpaused Menus can apply their normal menu behavior.

### Add Actor limitation / guarded fallback

OStim's `Add Actor` selection is implemented inside `PlayerThreadStarter.cpp` as an internal message-box callback that calls the non-exported `addActor(params, actor)` function. OStim's current public ModAPI does not expose a callback before that actor is committed.

Therefore 0.25.0 does **not** use a fragile version-specific binary hook for that internal lambda. If a scene begins with an NPC and a remote STR proxy is introduced later through OStim's `Add Actor` dialog, OStim Together retains the 0.24.1 guarded thread-preflight fallback:

```text
NPC flow / Furniture / Add Actor
→ proxy chosen
→ OStim finishes its pre-scene flow
→ disposable final preflight thread is suppressed by PreflightGuard
→ consent is requested
→ approved scene is recreated
```

This guarantees that an unconsented remote player cannot remain in the synchronized scene, but it does not yet pause the `Add Actor` dialog chain at the exact moment the proxy is selected. Implementing that exact UX safely requires either a new upstream OStim pre-add-actor API/event or a separately validated hook for each supported OStim binary.

## Guarded fallback preflight

`PreflightGuard` remains registered before `OStimBridge`. For any remote-proxy scene that bypasses the pre-UI gate, it marks the disposable thread as suppressed before OStimBridge can queue START/Wall/pose work. This prevents the delayed-work-after-stop crash fixed in 0.24.1.

Expected fallback diagnostics:

```text
OSTNET PREFLIGHT GUARD suppress thread=... reason=remote-consent-pending
OSTNET MIRROR suppress TX START localThread=...
OSTNET COOP PREFLIGHT CAPTURE ...
OSTNET COOP PREFLIGHT STOP ...
OSTNET COOP INVITE TX ... source=fallback-preflight
```

## Shared controls

The initiating player remains authoritative, but accepted remote participants can use normal OStim controls. Their local changes are forwarded as:

```text
CONTROL_NODE|thread=<owner thread>|node=<scene id>
CONTROL_SPEED|thread=<owner thread>|speed=<speed>
CONTROL_STOP|thread=<owner thread>
```

The owner applies NODE/SPEED/STOP through OStim's public ModAPI and the resulting authoritative state is fanned back out. Network-applied changes are echo-suppressed.

Speed reads are deferred outside OStim's SPEED callback to avoid reentrant OStim lock deadlocks.

## Other synchronization features

- targeted STRPM consent and scene traffic;
- exact OStim 7.5 furniture synchronization through Threads ABI v3 `getFurnitureObject()`;
- OStim 7.4c blocked-furniture fallback;
- delayed authoritative startup for Wall scenes;
- STR proxy pose stabilization during active scenes;
- equipment/outfit protection for dynamic STR proxies;
- residual apparel repair/restoration;
- RaceMenu/SKEE overlay registration and rebuild support;
- generic addon synchronization;
- optional OCum Ascended RaceMenu overlay and 3D equip-object synchronization;
- addon-state reapplication after OStim node/body rebuilds.

## STRPluginMessagingAPI

```text
OStim Together
    ↓ channel: ostimtogether
STRPluginMessagingAPI
    ↓
STRPluginMessagingBridge
    ↓
Skyrim Together Reborn
```

There is **no UDP fallback**.

Expected startup diagnostics:

```text
OSTNET STRPM READY channel=ostimtogether ... proxyResolver=1 ...
OSTNET PREFLIGHT GUARD READY priority=before-bridge ...
OSTNET COOP READY consent=safe-messagebox preUiGate=1 fallbackPreflight=1 sharedControls=1 stopAnyParticipant=1 ...
OSTNET INPUT GATE READY order=...before-ostim ...
```

## OStim compatibility

Validated compatibility layer targets:

- OStim 7.4c — `OStim.dll` `7.4.0.3`;
- OStim 7.5b — `OStim.dll` `7.5.0.2`.

## Optional OCum Ascended integration

The optional integration lives in `optional/OCumIntegration/`.

The FOMOD layout is:

- `00 Core` — always installed;
- `10 OCum Ascended` — optional.

## Configuration

```ini
[General]
ToggleKey=68
ClearKey=87
IntervalMs=25
DebugNotifications=1

[Equipment]
SlotMask=140
```

## Build

```powershell
$env:VCPKG_ROOT="C:\dev\vcpkg"
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
Remove-Item -Recurse -Force .\build -ErrorAction SilentlyContinue
.\build-vortex.ps1
```

Core output:

```text
dist/OStimTogether-v0.25.0-Core-Vortex.zip
```

FOMOD:

```powershell
.\build-fomod.ps1
```

Expected output:

```text
dist/OStimTogether-v0.25.0-FOMOD.zip
```

`release-fomod/` is generated staging and must not be committed.
