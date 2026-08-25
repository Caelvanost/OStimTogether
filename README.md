# OStim Together

Current development version: **0.37.5**.

OStim Together synchronizes OStim Standalone scenes between Skyrim Together Reborn players. The `strpm` branch uses **STRPluginMessagingAPI only**; there is no UDP fallback.

The root `VERSION` file is the single source of truth for CMake, DLL startup logs and archive names. Small fixes increment the patch number; larger feature/architecture work increments the minor number and resets the patch number.

## Current status

v0.37.5 is the release candidate following the final OCum 3D-mesh diagnostic.

### Core synchronization

OStim Together currently provides:

- player-to-player OStim scene start synchronization;
- player + NPC scene mirroring without forcing multiplayer-only alignment paths;
- exact OStim 7.5 furniture synchronization through Threads ABI v3;
- OStim 7.4c furniture fallback;
- wall-scene startup handling;
- shared native OStim NODE / SPEED / STOP controls;
- participant-authored OStim alignment synchronization;
- clock-calibrated free-standing scene phase replay;
- bounded mirror-self post-animation position correction;
- remote-proxy logical-origin correction for shared free scenes;
- equipment/outfit protection during scenes;
- safe orphan proxy-only OStim thread cleanup;
- targeted multiplayer consent / Add Actor gating;
- optional PPA runtime target synchronization;
- optional OCum Ascended RaceMenu overlay synchronization.

## OCum Ascended support

### Supported

OCum RaceMenu `CumOverlays` are synchronized between the true local player and the corresponding remote Skyrim Together proxy.

OStim Together keeps the true local PlayerCharacter fully owned by OCum/RaceMenu and mirrors the resulting overlay state read-only. The supported path includes the RaceMenu texture/decal properties and the live visibility metadata used by the remote proxy overlay bridge.

### Known limitation — OCum 3D meshes

OCum's OStim equip-object meshes:

```text
ocumvagmesh
ocumanmesh
```

are **not supported on remote Skyrim Together player proxies**.

The final v0.37.4 diagnostic proved that this is not a packet-loss or actor-resolution problem. On the receiving client:

```text
ADDONOBJ received
-> remote STR proxy resolved
-> OActor.EquipObject(...) returned true
-> OActor.IsObjectEquipped(...) returned true at T+250 ms
-> OActor.IsObjectEquipped(...) returned true again at T+1250 ms
-> 3D mesh still not rendered on the remote proxy
```

The same mesh remains visible on the true local player. This means OStim accepts and retains the equip-object state, but the remote STR proxy does not materialize/render that 3D equip object.

v0.37.5 therefore intentionally treats `ocumvagmesh` and `ocumanmesh` as **local-only** and suppresses their network synchronization. Repeated retries are not performed for these OCum meshes in the supported release path.

This limitation does **not** affect OCum RaceMenu texture overlays.

## Support matrix

| Feature | Status |
| --- | --- |
| OStim scene start / stop | Supported |
| OStim node changes | Supported |
| OStim speed changes | Supported |
| Shared scene control | Supported |
| Furniture scenes | Supported |
| Wall scenes | Supported |
| Free-standing alignment | Supported |
| PPA runtime target sync | Supported with optional integration |
| OCum RaceMenu `CumOverlays` | Supported with optional integration |
| OCum 3D vaginal mesh (`ocumvagmesh`) | **Not supported remotely** |
| OCum 3D anal mesh (`ocumanmesh`) | **Not supported remotely** |

## Compatibility

Validated OStim runtime layouts:

- OStim 7.4c — `7.4.0.3`;
- OStim 7.5b — `7.5.0.2`.

The free-scene phase/alignment specialization targets the validated OStim 7.5b Threads API v3 path. Furniture and wall handling retain their dedicated paths.

The mandatory Add Actor gate `OSKSE.pex` compatibility patch is based on the validated OStim 7.5b interface and must win its file conflict against OStim's original script.

For multiplayer testing and release use, both Skyrim Together clients should run the same OStim Together version and compatible OStim/OCum/PPA versions.

## Optional integrations

### OCum Ascended

The FOMOD option synchronizes supported OCum RaceMenu overlays. OCum 3D vaginal/anal equip-object meshes are deliberately excluded from the supported remote feature set.

### PPA — Procedural Penis Animations

The optional PPA integration synchronizes runtime penetration target information between scene participants. If the PPA runtime is missing or unsupported, OStim Together disables the integration safely.

## Build

v0.37.5 changes `OStimTogetherOCum.psc`, so compile the optional OCum integration first:

```powershell
.\optional\OCumIntegration\compile-ocum-integration.ps1 `
  -SkyrimDir "C:\Games\Steam\steamapps\common\Skyrim Special Edition"
```

Then build the release FOMOD:

```powershell
$env:VCPKG_ROOT="C:\dev\vcpkg"
.\build-fomod.ps1
```

Expected output:

```text
dist\OStimTogether-v0.37.5-FOMOD.zip
```

FOMOD layout:

- `00 Core` — required DLL/INI plus mandatory Add Actor consent scripts;
- `10 OCum Ascended` — optional supported OCum RaceMenu overlay integration;
- `20 Procedural Penis Animations` — optional PPA synchronization.

The FOMOD build checks that compiled Papyrus files are not older than their sources and will stop with an explicit message if recompilation is required.

## Development note

The v0.37.4 OCum 3D-object diagnostics remain useful as historical evidence in Git history, but v0.37.5 no longer presents those meshes as a supported feature. Future work may revisit them only if Skyrim Together or OStim changes how equip-object geometry is materialized on remote player proxies.
