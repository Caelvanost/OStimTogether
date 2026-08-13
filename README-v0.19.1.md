# OStim Together v0.19.1 — Core + optional integrations

v0.19.1 is an architectural split.

## Core

`OStimTogether.dll` contains only OStim/Skyrim Together synchronization and a
generic addon bus.  The C++ Core contains **no OCum-specific identifiers**.

Preserved from the known-good pre-OCum branch:

- automatic LAN UDP discovery;
- mirrored OStim START / NODE / STOP;
- STR player-proxy resolution;
- STR ownership of remote-player movement;
- local SELF one-shot correction only;
- exact furniture handling;
- Wall-only delayed START;
- EquipmentLock / DefaultOutfitGuard skip for likely STR player proxies;
- RaceMenu holder registration for dynamic STR proxies;
- bounded T100/T500 RaceMenu proxy overlay rebuilds only when a proxy holder was newly registered;
- CommonLib generated-file `std::literals` workaround.

## Generic addon bus

Papyrus integrations can send a standard SKSE ModEvent:

`ostimtogether_addon`

with one of these `strArg` formats:

- `OVR|<channel>|<texture-marker>`
- `OBJ|<channel>|<ostim-object-type>`

For `OBJ`, `numArg > 0.5` means equipped and `0` means unequipped.

The Core accepts addon messages only when the ModEvent sender is the true local
PlayerCharacter.  It then transmits the state to the peer and resolves that
character to the peer's dynamic STR proxy.

`OVR` captures complete RaceMenu Face / Body / Hands / Feet overlay slots whose
current texture contains the marker.  The receiver stores the node overrides,
requests one bounded RaceMenu overlay rebuild, and reapplies properties at T120
and T500.  The implementation never calls SKEE `GetNodeProperty()`.

`OBJ` mirrors an opaque OStim equip-object type through `OActor.EquipObject` /
`OActor.UnequipObject` on the remote proxy.

## Optional OCum Ascended integration

Source:

`optional/OCumIntegration/`

The optional quest script:

- listens to `ocum_applied_cum`;
- ignores events where the target is not the true local player;
- requests synchronization for textures containing `CumOverlays`;
- asks OStim for the **actual** `ocumvagmesh` / `ocumanmesh` state before
  transmitting those objects;
- does not invent a vaginal/anal mesh merely because a decal exists;
- does not create a fictitious facial mesh — facial/mouth cum is synchronized
  through RaceMenu overlays.

The binary `OStimTogether_OCum.esp` and `OStimTogetherOCum.pex` must be produced
with Creation Kit / PapyrusCompiler on Windows.  See:

`optional/OCumIntegration/CreationKit-ESP.md`

## FOMOD layout

The release FOMOD is designed as:

- `00 Core` — always installed;
- `10 OCum Ascended` — optional.

`build-fomod.ps1` builds the final installer after the optional ESP and PEX have
been placed in `optional/OCumIntegration/package/Data/`.

## Build Core

```powershell
$env:VCPKG_ROOT="C:\dev\vcpkg"
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass

Remove-Item -Recurse -Force .\build -ErrorAction SilentlyContinue
.\build-vortex.ps1
```

Expected Core package:

`OStimTogether-v0.19.1-Core-Vortex.zip`

## Build optional OCum script

```powershell
.\optional\OCumIntegration\compile-ocum-integration.ps1 `
    -SkyrimDir "C:\Games\Steam\steamapps\common\Skyrim Special Edition"
```

Then create/copy `OStimTogether_OCum.esp` according to
`CreationKit-ESP.md` and run:

```powershell
.\build-fomod.ps1
```

Expected release:

`OStimTogether-v0.19.1-FOMOD.zip`

## Source compile fix 1

- Fixed Papyrus syntax in `optional/OCumIntegration/Data/Scripts/Source/OStimTogetherOCum.psc`.
- `SendModEvent` calls are kept on a single line for compatibility with the Skyrim Papyrus compiler.
- Local mesh-state variables are declared at function scope before executable statements.

## v0.19.1 — STR remote-proxy position ownership fix

Locally-owned OStim scenes may include the other Skyrim Together player's
dynamic proxy. OStim's `lockAtPosition()` starts a persistent engine
`TranslateTo()` on every participant, while Skyrim Together is also updating
that proxy's transform. The two writers can produce visible back-and-forth
oscillation on the host client.

v0.19.1 keeps STR as the sole position authority for dynamic remote-player
proxies in locally-owned scenes. After each local START and NODE, a bounded
three-task defer waits for OStim's queued alignment work and then calls only
`StopTranslation()` on those dynamic proxies. It does not teleport them, does
not write their transform, and does not continuously pin them. The local real
PlayerCharacter and mirror-side authoritative SELF logic are unchanged.

Diagnostic log:

```text
OSTNET STR PROXY POSITION RELEASE ... action=stop-ostim-translation owner=STR
OSTNET STR PROXY POSITION OWNER ... owner=STR continuousPin=0
```
