# OStim Together

Current development version: **0.24.0**.

The root `VERSION` file is the single source of truth for the project version. CMake, the DLL startup log, the Vortex archive name and the FOMOD archive name are derived from it.

Versioning rule used by this project:

- small fixes/adjustments increment the third number;
- larger feature or architecture changes increment the second number and reset the third.

OStim Together is an SKSE plugin that synchronizes OStim Standalone scenes between Skyrim Together Reborn players. The `strpm` branch uses **STRPluginMessagingAPI** as its only multiplayer transport.

## Current cooperative architecture

For a local scene that contains one or more Skyrim Together remote-player proxies, 0.24.0 uses a preflight consent flow:

```text
OStim local thread is created
    ↓
OStim Together detects local player + STR proxy participant(s)
    ↓
preflight thread is stopped immediately after the OStim START callback returns
    ↓
all START/NODE/SPEED/STOP packets from that preflight thread are suppressed
    ↓
targeted INVITE is sent to the remote participant(s)
    ↓
remote player gets a Skyrim-Souls-safe native MessageBox
    ↓
Accept
    ↓
OStim Together recreates the authoritative local scene from the captured
actors + starting node + furniture reference
    ↓
normal authoritative START is sent to accepted participants only
```

With the public OStim API, participant/furniture information does not exist until a thread has been created. Therefore the implementation cannot prevent OStim from constructing the initial preflight thread internally, but that thread is stopped on the first safe game-task after the START callback and never becomes the multiplayer authoritative scene. The accepted replay is the scene that remains active and is synchronized.

Pure local player/NPC scenes are not preflighted. The consent path activates only when at least one dynamic STR player proxy can be resolved back to an STRPM `ConnectionID`.

## 0.24.0 — preflight consent + safe MessageBox

### Player 1 does not remain in scene while Player 2 decides

0.23.x captured consent only when the authoritative network `START` was ready. The initiator's OStim scene could therefore already be running while the remote player was deciding whether to accept.

0.24.0 moves consent to the OStim thread-start lifecycle. The first local player+proxy thread is treated as a disposable preflight and stopped immediately outside the OStim callback. Its network state is suppressed.

Expected owner-side diagnostics:

```text
OSTNET COOP PREFLIGHT CAPTURE session=... thread=... participants=... action=stop-before-consent
OSTNET COOP PREFLIGHT STOP session=... thread=... result=...
OSTNET COOP INVITE TX session=... participants=... localSceneActive=0
```

After acceptance:

```text
OSTNET COOP INVITE RESPONSE RX session=... accepted=1 start=1
OSTNET COOP APPROVED THREAD LIVE session=... thread=... node=...
OSTNET COOP APPROVED START session=... result=... returnedThread=...
```

### Skyrim Souls / Unpaused Menus safe MessageBox

The remote confirmation dialog reuses the approach validated in Trade Together.

OStim Together creates `MessageBoxData` through Skyrim's native `MessageDataFactoryManager`, fills only the body text/buttons/callback and leaves every other factory-initialized field untouched. In particular it does not hand-forge internal MessageBox state. This allows Skyrim Souls / Unpaused Menus to apply its registered menu creator and unpaused-menu flags normally.

Expected receiver diagnostics:

```text
OSTNET COOP INVITE MESSAGEBOX ownerConnection=... session=... sender="..."
OSTNET SAFE MESSAGEBOX queued buttons=2 nativeDefaults=1
OSTNET COOP INVITE RESPONSE TX ... accepted=1 source=safe-messagebox
```

Buttons are:

```text
Accept
Decline
```

The invitation times out after 30 seconds on the owner. A late response to a canceled session is ignored.

## Shared controls

The initiating player remains the authoritative OStim thread owner, but every accepted participant can use the normal OStim controls on their mirrored scene.

Remote navigation becomes:

```text
CONTROL_NODE|thread=<owner thread>|node=<scene id>
```

Remote speed changes become:

```text
CONTROL_SPEED|thread=<owner thread>|speed=<speed>
```

The owner validates that the sender is an accepted participant, applies the request through OStim's public ModAPI, then the ordinary authoritative `NODE`/`SPEED` state is sent back to all participants.

Network-applied changes are suppressed on mirror clients so they are not echoed back as new control requests.

Speed reads are always deferred outside OStim's SPEED callback. Calling `GetCurrentSpeed()` reentrantly from that callback can deadlock OStim, which was fixed in 0.23.2.

## Shared scene termination

If any remote participant ends their mirrored OStim scene, OStim Together sends:

```text
CONTROL_STOP|thread=<owner thread>
```

The owner stops the authoritative OStim thread and the resulting `STOP` is propagated to every participant. This prevents the old state where one client redressed locally while another client still saw that player's proxy continuing the scene.

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

Transport path:

```text
OStim Together
    ↓ channel: ostimtogether
STRPluginMessagingAPI
    ↓
STRPluginMessagingBridge
    ↓
Skyrim Together Reborn
```

Remote identity uses STRPM ProxyResolver in both directions:

```text
ConnectionID -> local STR proxy FormID
local STR proxy FormID -> ConnectionID
```

There is **no UDP fallback**.

Expected startup diagnostics:

```text
OSTNET STRPM READY channel=ostimtogether ... proxyResolver=1 ...
OSTNET COOP READY consent=safe-messagebox preflight=1 sharedControls=1 stopAnyParticipant=1 ...
```

## OStim compatibility

Validated compatibility layer targets:

- OStim 7.4c — `OStim.dll` `7.4.0.3`;
- OStim 7.5b — `OStim.dll` `7.5.0.2`.

OStim 7.5 Threads interface v3 is used for exact furniture access. Unknown internal graph layouts are rejected rather than read with unvalidated offsets.

## Optional OCum Ascended integration

The optional integration lives in:

```text
optional/OCumIntegration/
```

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

Multiplayer transport settings belong to STRPM/Skyrim Together Reborn.

## Build

Set `VCPKG_ROOT`, then:

```powershell
$env:VCPKG_ROOT="C:\dev\vcpkg"
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
Remove-Item -Recurse -Force .\build -ErrorAction SilentlyContinue
.\build-vortex.ps1
```

Core output for 0.24.0:

```text
dist/OStimTogether-v0.24.0-Core-Vortex.zip
```

FOMOD:

```powershell
.\build-fomod.ps1
```

Expected output:

```text
dist/OStimTogether-v0.24.0-FOMOD.zip
```

`release-fomod/` is generated staging and must not be committed.
