# OStim Together

Current development version: **0.22.2**.

The root `VERSION` file is the single source of truth for the project version. CMake, the DLL startup log, the Vortex archive name and the FOMOD archive name are derived from it.

Versioning rule used by this project:

- small fixes/adjustments increment the third number (`0.22.1` -> `0.22.2`);
- larger feature or architecture changes increment the second number and reset the third (`0.22.x` -> `0.23.0`).

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

## Current features

- mirrored OStim `START`, `NODE`, `SPEED`, and `STOP` state;
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
- bounded, generation-guarded retries for remote OCum 3D equip objects when their state arrives before the mirrored OStim actor is fully registered;
- cached remote addon state reapplication after OStim node changes so node/body rebuilds do not permanently remove synchronized addon visuals;
- OStim Standalone 7.4c and 7.5b graph-layout compatibility inherited from the previous codebase.

## 0.22.2 residual apparel and overlay materialization fix

The 0.22.1 runtime logs showed that the receiving player's body could already be stripped while a visually remaining helmet was not reported through the simple `Head` probe. 0.22.2 now enumerates the actual worn armor inventory and logs each worn item's full biped slot mask. Once OStim has already removed the body slot, residual apparel on standard head/hair/circlet/ears, hands/forearms, and feet/calves slots is given one final OStim undress pass and then forcibly unequipped if it remains. Only items removed by this repair are recorded, and they are restored after OStim finishes its normal scene-stop redress path.

Expected diagnostics include:

```text
OSTNET MIRROR WORN thread=... actor=00000014 item=... slots=0x........ residual=1
OSTNET MIRROR UNDRESS REPAIR thread=... action=OStim-undress+verify
OSTNET MIRROR RESIDUAL UNEQUIP thread=... actor=00000014 item=... slots=0x........
OSTNET MIRROR RESIDUAL RESTORE thread=... actor=00000014 item=...
```

For RaceMenu overlays, the receiving proxy already had a valid SKEE overlay holder and the synchronized OCum properties were reaching live body overlay materials. The previous refresh still queued RaceMenu's overlay rebuild asynchronously. 0.22.2 now executes `AddOverlays(actor, true)` from the scheduled game-thread refresh, forcing RaceMenu to rebuild/relink the registered proxy overlay geometry immediately and reapply its stored node overrides against the current OStim/STR body geometry.

Expected diagnostics now use:

```text
OSTNET SKEE OVERLAY REBUILD reason=ADDON-OBJECT phase=T80 actor=... hadOverlays=1 immediate=1
```

## 0.22.1 compile fix

0.22.1 adjusts the mirror residual-armor probe for the CommonLibSSE-NG 3.5.3 API used by the project toolchain. `Actor::GetWornArmor` is called with the single slot argument exposed by that version.

## 0.22.0 synchronization repairs

### Exact furniture on OStim 7.5

OStim 7.5 exposes the exact furniture reference owned by a thread through the public Threads ABI v3. OStim Together now uses that reference directly instead of deciding whether a blocked nearby furniture reference is valid from the gameplay `RE::Actor` position.

This matters because OStim can already be rendering a player on furniture while the underlying gameplay actor reference is still hundreds of Skyrim units away during the initial START callback. The previous distance filter could therefore reject the correct bench/table and send `furniture=none` to the remote client.

Expected diagnostic on OStim 7.5:

```text
OSTNET FURNITURE THREAD EXACT node=... ref=... base=... interfaceV3=1
```

### Mirror residual-armor repair

A mirrored scene can occasionally leave one primary armor piece equipped on the receiving player's real local character even though OStim already stripped the body. This can also keep unrelated visual systems such as IED in an armored/display state.

OStim Together checks the receiving local player shortly after a synchronized multi-player OStim thread starts. Fully clothed scenes are not force-undressed.

### Remote addon lifecycle repair

OStim can rebuild actor geometry and change GraphActor equip-object requirements during a node change. A synchronized OCum mesh or RaceMenu overlay that was correct immediately after receipt can therefore disappear later in the same scene.

OStim Together:

- reapplies cached remote addon state after OStim node changes;
- keeps object-state retries generation-guarded so stale state cannot overwrite a newer state;
- rebuilds SKEE overlay geometry after remote addon properties have been stored, allowing an already-existing dynamic proxy overlay holder to relink to current body geometry without deleting unrelated overlays.

Expected diagnostics include:

```text
AddonStateRepair READY threadsVersion=3
OSTNET ADDON NODE REPAIR thread=... node=...
OSTNET ADDON STATE REAPPLY reason=OSTIM-NODE phase=T250 ...
```

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

The compatibility layer supports the tested OStim Standalone runtimes:

- OStim 7.4c — `OStim.dll` `7.4.0.3`;
- OStim 7.5b — `OStim.dll` `7.5.0.2`.

OStim 7.5 changed the internal graph actor layout and exposes Threads interface version 3. OStim Together selects the validated graph layout from the loaded OStim DLL version, uses v3-only public furniture accessors only when the runtime reports support for them, and rejects unknown layouts instead of reading unverified offsets.

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

On a receiving STR client, an OCum object-state packet can arrive a few frames before OStim has fully registered the remote proxy in its mirrored thread. OStim Together therefore retries a newly changed object state for a short bounded window. Retries are guarded by a per-actor/object generation so an older state cannot override a newer one.

The FOMOD layout is:

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

With `VERSION` set to `0.22.2`, the Core output is:

```text
dist/OStimTogether-v0.22.2-Core-Vortex.zip
```

For the optional OCum integration, produce/copy its ESP and PEX as documented under `optional/OCumIntegration/`, then run:

```powershell
.\build-fomod.ps1
```

The FOMOD output is:

```text
dist/OStimTogether-v0.22.2-FOMOD.zip
```

Changing `VERSION` automatically changes both archive names, the CMake project version and the version reported by the DLL at startup. `build-fomod.ps1` also stamps the staged FOMOD `info.xml` with the same value.

`release-fomod/` is a generated staging directory. It is recreated by `build-fomod.ps1`, is ignored by Git, and must not be committed.

## Migration note

The old custom UDP implementation was never considered a validated transport baseline. On the `strpm` branch it has been removed from the build/runtime path. A temporary source-level compatibility alias named `UdpTransport` remains only so older addon call sites can compile while they are renamed; it performs no socket, discovery, relay, authentication or UDP operation and delegates sends exclusively to STRPM.
