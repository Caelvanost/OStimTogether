# OStim Together

Current development version: **0.23.2**.

The root `VERSION` file is the single source of truth for the project version. CMake, the DLL startup log, the Vortex archive name and the FOMOD archive name are derived from it.

Versioning rule used by this project:

- small fixes/adjustments increment the third number (`0.23.1` -> `0.23.2`);
- larger feature or architecture changes increment the second number and reset the third (`0.23.x` -> `0.24.0`).

OStim Together is an SKSE plugin that synchronizes OStim Standalone scenes between Skyrim Together Reborn players. The `strpm` branch delegates multiplayer transport and remote-player identity to **STRPluginMessagingAPI**.

## STRPM-only architecture

OStim Together does not open its own UDP socket and does not provide LAN discovery, direct-connect reuse, manual `RemotePeers`, relay-host mode, port forwarding, or OStim-specific network authentication.

Transport path:

```text
OStim Together
    ↓ channel: ostimtogether
STRPluginMessagingAPI
    ↓
STRPluginMessagingBridge
    ↓
Skyrim Together Reborn
    ↓
STR server + STRPM relay resource
```

Remote player identity uses STRPM ProxyResolver:

```text
STRPM ConnectionID
    ↕
ProxyResolver
    ↕
local Skyrim Together proxy FormID
```

When STRPM is unavailable or not connected, OStim Together drops multiplayer synchronization events. **There is no UDP fallback.**

## Current features

- mirrored OStim `START`, `NODE`, `SPEED`, and `STOP` state;
- non-modal remote consent before a player's local character is entered into another player's synchronized OStim scene;
- targeted STRPM scene traffic to the actual remote participants instead of broadcasting cooperative scene state to every connected player;
- shared animation navigation and speed controls: any participant can control the scene, while the initiator remains the authoritative state owner;
- shared scene termination: if any participant stops their local mirrored scene, a `CONTROL_STOP` request stops the authoritative OStim thread and the resulting `STOP` is replicated to every participant;
- only the locally owned player thread is transmitted; auxiliary/NPC-only OStim setup threads are not mirrored;
- STRPM-based sender identity and remote-player proxy FormID resolution;
- exact OStim 7.5 furniture synchronization through the public Threads ABI v3 `getFurnitureObject()` accessor;
- OStim 7.4c compatibility fallback using the older blocked-furniture scan;
- delayed authoritative startup for Wall scenes whose final OStim anchor settles after scene creation;
- active-scene STR proxy pose stabilization using the authoritative OStim actor pose;
- equipment/outfit protection that avoids mutating dynamic STR player bases;
- targeted mirror residual-apparel repair after OStim has stripped the body, including secondary head/hair/circlet and limb slots, with restoration after scene stop;
- RaceMenu overlay registration, persistence, live-property application, OverlayFix-aware un-culling, and bounded SKEE geometry rebuilds for dynamic STR proxies;
- generic addon synchronization through the `ostimtogether_addon` ModEvent;
- optional OCum Ascended synchronization for marked RaceMenu overlays and OCum 3D equip objects;
- bounded, generation-guarded retries for remote OCum 3D equip objects;
- cached remote addon state reapplication after OStim node changes;
- OStim Standalone 7.4c and 7.5b graph-layout compatibility.

## 0.23.2 cooperative speed callback deadlock fix

0.23.1 introduced the non-modal consent path successfully, but its new cooperative speed listener called OStim ModAPI `GetCurrentSpeed()` directly from OStim's speed-change callback. OStim can still hold its internal thread lock while notifying speed listeners, so re-entering the ModAPI from that callback can deadlock the game. A local Player/NPC scene exposed the regression immediately: the log ended after the first OStim SPEED event, before any network START or consent traffic.

0.23.2 applies the same callback-safety rule already used by `OStimBridge::HandleSpeed()`:

- ordinary local, NPC, and auxiliary threads are rejected from the cooperative speed path **before any OStim ModAPI call**;
- cooperative mirror speed events only enqueue a Skyrim task;
- `GetCurrentSpeed()` runs later, after the OStim listener has returned and its internal lock has unwound;
- the existing speed echo-suppression and `CONTROL_SPEED` routing then run from that deferred task;
- the mirror START callback no longer calls `GetCurrentSpeed()` either.

Expected cooperative diagnostic after a participant changes speed:

```text
OSTNET COOP CONTROL SPEED TX DEFERRED localThread=... ownerConnection=... ownerThread=... speed=...
```

A normal local Player/NPC scene should produce no cooperative speed-control log and must remain entirely on OStim's normal local path.

## 0.23.1 non-modal consent / startup-freeze fix

0.23.0 initially used a native Skyrim `MessageBox` for remote consent and transmitted the invitation directly while still unwinding OStim's START/fade path. With Skyrim Souls / Unpaused Menus this could freeze the initiating client during the fade before the remote player ever received the consent dialog.

0.23.1 removes the modal menu path completely.

The authoritative START callback now only captures the cooperative session and returns to OStim. The actual targeted STRPM `INVITE` is dispatched **150 ms later** from Skyrim's task queue, after the OStim callback/fade-start path has unwound.

Expected sender-side diagnostics:

```text
OSTNET COOP START CAPTURED thread=... participants=... dispatchDelayMs=150 callbackSafe=1
OSTNET COOP INVITE TX DEFERRED thread=... participants=... status=pending
```

The receiving player now gets a normal non-blocking notification:

```text
OStim Together: <player> invites you - Y accept / N decline
```

No `MessageBoxData`, UIExtensions dialog, paused menu, or modal callback is created. This is intended to remain compatible with Skyrim Souls / Unpaused Menus.

Default DirectInput scan codes are:

```ini
ConsentAcceptKey=21
ConsentDeclineKey=49
```

which correspond to **Y** and **N** on the default keyboard mapping. These keys are only consumed while an OStim Together invitation is pending; otherwise they behave normally.

The invitation still expires after 30 seconds. Timeout, decline, shared controls and shared STOP retain the 0.23.0 cooperative-session behavior.

Expected receiver diagnostics:

```text
OSTNET COOP INVITE NONMODAL ownerConnection=... thread=... sender="..." acceptKey=21 declineKey=49
OSTNET COOP INVITE RESPONSE TX ... accepted=1 source=keyboard
```

## 0.23.0 cooperative scene sessions

### Consent before the remote mirror starts

When the authoritative local OStim thread contains one or more STR remote-player proxies, OStim Together resolves each proxy back to its STRPM `ConnectionID` and sends a targeted invitation instead of immediately sending `START`.

The initiator's local OStim thread may already exist while consent is pending, but **the remote mirror is not created until all remote participants accept**.

If a participant declines or the request is unanswered for 30 seconds, OStim Together cancels the invitation and stops the initiating local scene.

### Shared animation controls

The initiating player remains the **authoritative OStim thread owner**, but remote participants can use their normal OStim controls on the mirrored scene.

A remote local node/speed change is converted to a targeted request:

```text
CONTROL_NODE|thread=<owner thread>|node=<scene id>
CONTROL_SPEED|thread=<owner thread>|speed=<speed>
```

The owner validates that the sender is an accepted participant, applies the request through OStim's public ModAPI, and the ordinary authoritative `NODE` or `SPEED` packet then fans out to all participants.

Network-applied node/speed changes are marked as expected mirror updates and are not echoed back as new control requests.

### Any participant can end the scene

If a remote participant ends their mirrored OStim scene, OStim Together sends:

```text
CONTROL_STOP|thread=<owner thread>
```

The owner calls OStim `StopScene()` on the authoritative thread. The normal authoritative `STOP` then reaches every participant. This fixes the old behavior where the remote player's local scene could end and redress while the initiator still saw that player's proxy continuing the scene.

## 0.22.x synchronization repairs

0.22.x introduced exact OStim 7.5 furniture ownership, mirror residual-apparel cleanup/restoration, RaceMenu/SKEE proxy overlay materialization, and OCum addon-state reapplication after OStim node/body rebuilds.

Key diagnostics include:

```text
OSTNET FURNITURE THREAD EXACT node=... ref=... interfaceV3=1
OSTNET MIRROR RESIDUAL UNEQUIP ...
OSTNET MIRROR RESIDUAL RESTORE ...
OSTNET SKEE OVERLAY REBUILD ... immediate=1
OSTNET ADDON NODE REPAIR ...
```

## STRPluginMessagingAPI requirement

This branch targets the validated STRPluginMessagingAPI v0.8.x contract. STRPM must be installed on each client and its required server relay resource must be installed on the Skyrim Together server.

OStim Together uses both directions of the proxy mapping:

- incoming `ConnectionID -> local proxy FormID` for sender identity;
- local proxy FormID -> remote `ConnectionID` for targeted consent and cooperative scene traffic.

Expected startup diagnostics include:

```text
OSTNET STRPM READY channel=ostimtogether ... proxyResolver=1 ...
OSTNET COOP READY consent=nonmodal sharedControls=1 stopAnyParticipant=1 ...
```

## OStim compatibility

The compatibility layer supports the tested OStim Standalone runtimes:

- OStim 7.4c — `OStim.dll` `7.4.0.3`;
- OStim 7.5b — `OStim.dll` `7.5.0.2`.

OStim 7.5 exposes Threads interface version 3. OStim Together selects the validated graph layout from the loaded OStim DLL version and rejects unknown layouts instead of reading unverified offsets.

## Generic addon bus

Papyrus integrations send the SKSE ModEvent `ostimtogether_addon` using:

- `OVR|<channel>|<texture-marker>`
- `OBJ|<channel>|<ostim-object-type>`

For `OBJ`, `numArg > 0.5` means equipped and `0` means unequipped.

The local real PlayerCharacter remains authoritative. Remote addon state is applied to the STR proxy resolved through the same STRPM identity path.

## Optional OCum Ascended integration

The optional integration lives in:

```text
optional/OCumIntegration/
```

It listens to `ocum_applied_cum`, synchronizes RaceMenu textures containing `CumOverlays`, and mirrors OCum 3D equip-object state.

The FOMOD layout is:

- `00 Core` — always installed;
- `10 OCum Ascended` — optional.

## Configuration

`OStimTogether.ini` contains only local plugin settings:

```ini
[General]
ToggleKey=68
ClearKey=87
ConsentAcceptKey=21
ConsentDeclineKey=49
IntervalMs=25
DebugNotifications=1

[Equipment]
SlotMask=140
```

All multiplayer transport configuration belongs to STRPM/Skyrim Together Reborn.

## Build

Set `VCPKG_ROOT`, then build from PowerShell:

```powershell
$env:VCPKG_ROOT="C:\dev\vcpkg"
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
Remove-Item -Recurse -Force .\build -ErrorAction SilentlyContinue
.\build-vortex.ps1
```

With `VERSION` set to `0.23.2`, the Core output is:

```text
dist/OStimTogether-v0.23.2-Core-Vortex.zip
```

For the optional OCum integration, produce/copy its ESP and PEX as documented under `optional/OCumIntegration/`, then run:

```powershell
.\build-fomod.ps1
```

The FOMOD output is:

```text
dist/OStimTogether-v0.23.2-FOMOD.zip
```

Changing `VERSION` automatically changes both archive names, the CMake project version and the version reported by the DLL at startup. `build-fomod.ps1` also stamps the staged FOMOD `info.xml` with the same value.

`release-fomod/` is a generated staging directory. It is recreated by `build-fomod.ps1`, is ignored by Git, and must not be committed.
