# OStim Together

Current STRPM development version: **0.20.1-strpm**.

OStim Together is an SKSE plugin that synchronizes OStim Standalone scenes between Skyrim Together Reborn players. The `strpm` branch keeps the existing OStim Together scene, positioning, furniture, RaceMenu and optional OCum functionality, but delegates multiplayer transport and remote-player identity to **STRPluginMessagingAPI**.

## STRPM-only architecture

OStim Together no longer opens its own UDP socket and no longer provides LAN discovery, direct-connect reuse, manual `RemotePeers`, relay-host mode, port forwarding, or OStim-specific network authentication.

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
STRPM sender ConnectionID
    ↓
ProxyResolver
    ↓
local Skyrim Together proxy FormID
```

When STRPM is unavailable or not connected, OStim Together logs the failure and drops multiplayer synchronization events. **There is no UDP fallback.**

## Current features retained

- mirrored OStim `START`, `NODE`, `SPEED`, and `STOP` state;
- STRPM-based sender identity and remote-player proxy FormID resolution;
- exact furniture synchronization when a placed furniture reference is available;
- delayed authoritative startup for Wall scenes whose final OStim anchor settles after scene creation;
- active-scene STR proxy pose stabilization using the authoritative OStim actor pose;
- equipment/outfit protection that avoids mutating dynamic STR player bases;
- RaceMenu overlay registration, persistence, live-property application, and OverlayFix-aware un-culling;
- generic addon synchronization through the `ostimtogether_addon` ModEvent;
- optional OCum Ascended synchronization for marked RaceMenu overlays and vaginal/anal equip objects;
- OStim Standalone 7.4c and 7.5b graph-layout compatibility from the existing v0.20.x codebase.

## STRPluginMessagingAPI requirement

This branch targets the validated STRPluginMessagingAPI v0.8.x contract. STRPM must be installed on each client and its required server relay resource must be installed on the Skyrim Together server.

The public ProxyResolver contract is used instead of guessing dynamic proxies through actor names or `ProcessLists` for STRPM-identified remote players.

Expected OStim Together startup diagnostics include:

```text
OSTNET STRPM READY channel=ostimtogether ... proxyResolver=1 ...
```

A received message should include its STR identity and resolved proxy:

```text
OSTNET STRPM RX connection=... sender="..." ... proxy=FF......
```

If STRPM cannot be loaded:

```text
OSTNET STRPM unavailable: multiplayer synchronization disabled; no UDP fallback
```

## OStim compatibility

The inherited v0.20.x compatibility layer supports the tested OStim Standalone runtimes:

- OStim 7.4c — `OStim.dll` `7.4.0.3`;
- OStim 7.5b — `OStim.dll` `7.5.0.2`.

OStim 7.5 changed the internal graph actor layout. OStim Together selects the validated layout from the loaded OStim DLL version and rejects unknown layouts instead of reading an unverified offset.

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

It listens to `ocum_applied_cum`, synchronizes RaceMenu textures containing `CumOverlays`, and mirrors the actual `ocumvagmesh` / `ocumanmesh` equipped state.

The FOMOD layout is now only:

- `00 Core` — always installed;
- `10 OCum Ascended` — optional.

There is no OStim Together relay-host component.

## Configuration

`OStimTogether.ini` contains only local plugin settings:

```ini
[General]
ToggleKey=68
ClearKey=87
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

Core output:

```text
dist/OStimTogether-v0.20.1-Core-Vortex.zip
```

For the optional OCum integration, produce/copy its ESP and PEX as documented under `optional/OCumIntegration/`, then run:

```powershell
.\build-fomod.ps1
```

FOMOD output:

```text
dist/OStimTogether-v0.20.1-FOMOD.zip
```

## Migration note

The old custom UDP implementation was never considered a validated transport baseline. On the `strpm` branch it has been removed from the build/runtime path. A temporary source-level compatibility facade named `UdpTransport` remains only so older addon call sites can compile while they are renamed; it performs no socket, discovery, relay, authentication or UDP operation and delegates sends exclusively to STRPM.
