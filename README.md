# OStim Together

Current development version: **0.20.1**.

OStim Together is an SKSE plugin that synchronizes OStim Standalone scenes between Skyrim Together Reborn players. It mirrors scene lifecycle, animation state, actor placement, and selected visual addon state while keeping Skyrim Together's dynamic player proxies stable during synchronized scenes.

## Current features

- automatic LAN peer discovery over UDP port `27991`;
- remote Internet peers through STR direct-connect reuse or explicit `RemotePeers` entries;
- optional Player1 relay-host mode for Internet sessions;
- optional HMAC-SHA256 authentication with `SharedSecret`;
- mirrored OStim `START`, `NODE`, `SPEED`, and `STOP` state;
- Skyrim Together player-proxy resolution on the receiving client;
- exact furniture synchronization when a placed furniture reference is available;
- delayed authoritative startup for Wall scenes whose final OStim anchor settles after scene creation;
- active-scene STR proxy pose stabilization using the authoritative OStim actor pose;
- equipment/outfit protection that avoids mutating dynamic STR player bases;
- RaceMenu overlay registration, persistence, live-property application, and OverlayFix-aware un-culling for synchronized marked overlays;
- generic addon synchronization through the `ostimtogether_addon` ModEvent;
- optional OCum Ascended integration for marked RaceMenu overlays and vaginal/anal equip objects.

## OStim compatibility

v0.20.1 supports the tested OStim Standalone runtimes:

- OStim 7.4c — `OStim.dll` `7.4.0.3`;
- OStim 7.5b — `OStim.dll` `7.5.0.2`.

OStim 7.5 changed the internal graph actor layout, moving the per-actor graph offset. OStim Together selects a separate read-only layout from the loaded DLL version before scene callbacks are registered. Unknown OStim DLL versions are rejected instead of reading an unverified memory offset.

Expected startup diagnostics on OStim 7.5b:

```text
OStim runtime DLL version=7.5.0.2 graphLayout=OStim-7.5b
OStim Threads interface version=3
```

## Network connections

### LAN

LAN discovery remains enabled by default through UDP broadcast on port `27991`.

### Internet / distant connection

Remote clients can keep `AutoDiscovery=1` and `AutoRemoteFromSTR=1`. When a client has connected to Skyrim Together Reborn by direct connect, OStim Together reads STR's saved host address and contacts that same host on UDP `27991`.

`RemotePeers=` accepts comma/semicolon-separated IPv4 addresses or DNS names, with optional per-peer ports. Legacy `PeerHost` / `PeerPort` remains supported and is added to `RemotePeers`.

### Player1 relay host

The FOMOD includes an Internet role step:

- **Client / LAN / no Internet relay** — normal players;
- **Player1 host / Internet relay - requires port forwarding** — only the Skyrim Together host / Player1 machine.

For the relay host:

1. Forward UDP port `27991` from the router to the Player1 PC.
2. Allow UDP `27991` through Windows Firewall.
3. Install the FOMOD relay-host option on that PC only.

The relay host learns each client's observed source endpoint and forwards gameplay packets between known peers. Remote clients normally do not need their own port forward as long as they can send outbound UDP to the host.

### Optional authentication

`SharedSecret=` can be set on every client to require HMAC-SHA256 on discovery and gameplay packets. `AutoSharedSecretFromSTR=1` can reuse STR's saved direct-connect password on clients and `STServer.ini`'s `sPassword` on the relay host when available.

Authentication remains off by default for the simplest initial remote setup.

Expected remote-client diagnostics:

```text
OSTNET STR auto remote configured address="host:10578" endpoint=x.x.x.x:27991
UDP transport started AUTO=1 RELAY=0 AUTH=0 client="..." port=27991 configuredPeers=1 ...
```

Expected Player1 relay-host diagnostics:

```text
UDP transport started AUTO=0 RELAY=1 AUTH=0 client="..." port=27991 ...
OSTNET DISCOVERED peer="..." addr=x.x.x.x:yyyyy instance=...
OSTNET RELAY source=x.x.x.x:yyyyy peers=... sender="..."
```

## Generic addon bus

Papyrus integrations can send the SKSE ModEvent `ostimtogether_addon` with one of these `strArg` formats:

- `OVR|<channel>|<texture-marker>`
- `OBJ|<channel>|<ostim-object-type>`

For `OBJ`, `numArg > 0.5` means equipped and `0` means unequipped.

The core accepts addon messages only when the sender is the true local PlayerCharacter. The state is then transported to the peer and applied to that player's dynamic STR proxy.

`OVR` captures marked RaceMenu Face / Body / Hands / Feet overlay properties and reapplies them to the live proxy geometry. `OBJ` mirrors opaque OStim equip-object types through OStim's actor API.

## Optional OCum Ascended integration

The optional integration lives in:

```text
optional/OCumIntegration/
```

It listens to `ocum_applied_cum`, synchronizes RaceMenu textures containing `CumOverlays`, and mirrors the actual `ocumvagmesh` / `ocumanmesh` equipped state. Facial and mouth cum are handled as RaceMenu overlays rather than a fictitious facial mesh.

The optional `OStimTogether_OCum.esp` and `OStimTogetherOCum.pex` must be produced with Creation Kit / PapyrusCompiler on Windows. See `optional/OCumIntegration/CreationKit-ESP.md`.

The FOMOD layout is:

- `00 Core` — always installed;
- `10 OCum Ascended` — optional;
- `20 Internet Relay Host` — optional, Player1 host only.

## Build

Set `VCPKG_ROOT`, then build from PowerShell:

```powershell
$env:VCPKG_ROOT="C:\dev\vcpkg"
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass

Remove-Item -Recurse -Force .\build -ErrorAction SilentlyContinue
.\build-vortex.ps1
```

The Core archive is written to:

```text
dist/OStimTogether-v0.20.1-Core-Vortex.zip
```

To compile the optional OCum Papyrus integration:

```powershell
.\optional\OCumIntegration\compile-ocum-integration.ps1 `
    -SkyrimDir "C:\Games\Steam\steamapps\common\Skyrim Special Edition"
```

After producing/copying the optional ESP and PEX according to `CreationKit-ESP.md`, build the FOMOD:

```powershell
.\build-fomod.ps1
```

The FOMOD archive is written to:

```text
dist/OStimTogether-v0.20.1-FOMOD.zip
```

`package/` remains the Core staging directory and `release-fomod/` remains the assembled FOMOD staging directory. Only final ZIP archives are written to `dist/`.

## Changelog

Older entries document the development path and may describe diagnostics or ownership strategies that were superseded by later releases.

### v0.20.1 — remote Internet connection and relay mode

- retained LAN discovery while adding remote peers through STR direct-connect address reuse or `RemotePeers`;
- added DNS/IPv4 remote endpoints with optional per-peer ports and legacy `PeerHost` compatibility;
- added a Player1 Internet relay-host FOMOD profile and peer forwarding;
- added optional HMAC-SHA256 packet authentication through `SharedSecret` / STR password reuse;
- retained v0.20.0 OStim 7.5b compatibility and OCum overlay fixes.

### v0.20.0 — OStim 7.5b compatibility

- added explicit OStim 7.5b graph-layout support while retaining OStim 7.4c support;
- selects the graph layout from the loaded `OStim.dll` version;
- rejects unknown OStim DLL versions rather than reading unverified offsets;
- retained the v0.19.6 proxy stability, cum-mesh synchronization, and overlay handling.

### v0.19.6 — OverlayFix-aware OCum rendering

- clears stale app-culling on the exact synchronized OCum overlay subtree after applying live properties;
- repeats the un-cull during deferred live reapplication passes;
- stops rebuilding an overlay holder when one already exists.

### v0.19.5 — per-frame STR proxy pose authority

- replaced the ineffective virtual-only position correction with a `TESObjectREFR` position path plus loaded-3D update;
- coalesces proxy corrections to at most one game-thread task per rendered frame;
- applies the same bounded active-scene pose guard to authoritative and mirror views.

### v0.19.4 — proxy guard and live overlay application

- introduced temporary OStim pose authority for dynamic STR proxies during locally owned scenes;
- applies received RaceMenu properties directly to existing proxy geometry in addition to persisting overrides;
- added deferred live overlay reapplication after RaceMenu rebuild timing windows.

### v0.19.3 — scene-launch deadlock hotfix

- stopped calling OStim `GetCurrentSpeed()` synchronously from the speed listener;
- defers the speed read and transmission to the next SKSE game task, avoiding re-entry into OStim's internal thread lock.

### v0.19.2 — speed and OCum synchronization

- synchronized authoritative OStim speed changes to mirrored scenes;
- released stale OStim translation ownership after speed replays;
- applied captured RaceMenu overrides to live 3D nodes;
- detected OCum vaginal/anal meshes from their real equipped armor forms;
- reapplied cached addon state after scene cleanup.

### v0.19.1 — initial STR position-ownership fix

- added a bounded post-`START` / post-`NODE` translation release for dynamic remote-player proxies;
- attempted to leave STR as sole position authority after OStim's queued alignment work;
- this strategy was later superseded by the active-scene pose guard introduced in v0.19.4/v0.19.5.

### v0.19.0 — Core and optional integrations split

- separated the generic C++ synchronization core from OCum-specific Papyrus integration;
- introduced the generic `OVR` / `OBJ` addon bus;
- added the optional OCum Ascended FOMOD component;
- preserved LAN discovery, mirrored scene lifecycle, furniture handling, Wall handling, proxy guards, and RaceMenu registration.

### v0.18.17 — RaceMenu proxy overlays

- registered dynamic STR proxies with RaceMenu and added bounded overlay rebuild diagnostics;
- established the path toward explicit NiOverride/overlay-state synchronization.

### v0.18.16 — proxy equipment diagnostic and Wall root probe

- made automatic `EquipmentLock` skip likely dynamic STR player proxies as a visual-state diagnostic;
- removed the ineffective v0.18.15 boot reassert;
- added receiver-side reference-vs-rendered-root diagnostics for Wall scenes.

### v0.18.15 — Wall mirror boot reassert diagnostic

- added an additional deferred authoritative SELF correction during mirrored Wall startup;
- the diagnostic was removed in v0.18.16 after proving ineffective.

### v0.18.14 — STR proxy appearance guard

- made `DefaultOutfitGuard` skip actors whose dynamic `FFxxxxxx` reference and dynamic TESNPC base identify likely STR remote-player proxies;
- avoided mutating the dynamic base that participates in remote RaceMenu/NiOverride appearance state.

### v0.18.13 — delayed authoritative Wall START

- delayed authoritative Wall `START` transmission by about one second so OStim's final scene anchor could settle before the mirror was created;
- recomputed the center and actor poses at transmission time.

### v0.18.12 — Wall START SELF reassert

- added a one-shot authoritative SELF pose correction after initial animation replay for Wall starting nodes.

### v0.18.11 — exact furniture SELF pre-anchor

- pre-anchored local SELF to the raw synchronized furniture reference before mirror startup;
- allowed OStim to apply its own furniture offset, scale, and final alignment from a nearby deterministic staging position.
