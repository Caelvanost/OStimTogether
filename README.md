# OStim Together

Current development version: **0.24.1**.

The root `VERSION` file is the single source of truth for the project version. CMake, the DLL startup log, the Vortex archive name and the FOMOD archive name are derived from it.

Versioning rule used by this project:

- small fixes/adjustments increment the third number;
- larger feature or architecture changes increment the second number and reset the third.

OStim Together is an SKSE plugin that synchronizes OStim Standalone scenes between Skyrim Together Reborn players. The `strpm` branch uses **STRPluginMessagingAPI** as its only multiplayer transport.

## Current cooperative architecture

For a local scene containing one or more STR remote-player proxies:

```text
OStim creates the initial local thread
    ↓
PreflightGuard runs before OStimBridge
    ↓
thread is classified as consent-preflight before authoritative START work is armed
    ↓
CoopSessionManager captures actors + node + furniture
    ↓
preflight thread is stopped on the next Skyrim task
    ↓
INVITE is sent to remote participant(s)
    ↓
remote player receives a Skyrim-Souls-safe native MessageBox
    ↓
Accept
    ↓
OStim Together recreates the authoritative scene
    ↓
normal START/NODE/SPEED/STOP synchronization begins
```

Pure local player/NPC scenes are not preflighted. The consent path activates only when a non-player actor resolves to an STRPM `ConnectionID`.

## 0.24.1 — guarded preflight crash fix

0.24.0 stopped the disposable preflight correctly, but OStimBridge's START listener had already run first and armed delayed authoritative work such as Wall startup alignment, proxy pose ownership and other START follow-ups. Those tasks could execute after the preflight thread had been destroyed and crash the initiating client.

0.24.1 adds a dedicated `PreflightGuard` START listener registered **before OStimBridge**. It recognizes a local-player + mapped STR-proxy thread and marks it as suppressed before OStimBridge sees the START event. The accepted replay is explicitly exempted so it becomes the normal authoritative thread.

Expected diagnostics:

```text
OSTNET PREFLIGHT GUARD READY priority=before-bridge ...
OSTNET PREFLIGHT GUARD suppress thread=0 reason=remote-consent-pending
OStim thread START id=0 actors=2 mirror=1
OSTNET MIRROR suppress TX START localThread=0
OSTNET COOP PREFLIGHT CAPTURE ...
OSTNET COOP PREFLIGHT STOP ... result=0
OSTNET COOP INVITE TX ... localSceneActive=0
```

After acceptance:

```text
OSTNET PREFLIGHT GUARD allow approved-replay thread=...
OSTNET COOP APPROVED THREAD LIVE ...
OSTNET COOP APPROVED START ...
```

## 0.24.0 — consent before persistent local scene

0.24.0 introduced a disposable preflight thread so Player1 does not remain in an active synchronized scene while Player2 is deciding whether to accept. With the public OStim API, actors/furniture are only available after OStim constructs a thread, so the implementation captures that initial thread, stops it outside the OStim callback, and recreates the approved scene after consent.

Remote consent uses the same safe native `MessageBoxData` pattern validated in Trade Together: `MessageDataFactoryManager` creates the object, OStim Together fills only body text/buttons/callback, and all runtime-initialized fields are preserved so Skyrim Souls / Unpaused Menus can apply their normal menu flags.

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
OSTNET COOP READY consent=safe-messagebox preflight=1 sharedControls=1 stopAnyParticipant=1 ...
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
dist/OStimTogether-v0.24.1-Core-Vortex.zip
```

FOMOD:

```powershell
.\build-fomod.ps1
```

Expected output:

```text
dist/OStimTogether-v0.24.1-FOMOD.zip
```

`release-fomod/` is generated staging and must not be committed.
