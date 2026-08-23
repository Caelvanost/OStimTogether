# Optional OCum Ascended Integration

This component is intentionally outside the OStimTogether core.

## Source of truth

Appearance authority belongs to the true local PlayerCharacter. Remote STR
proxies are ignored as senders.

0.31.5 uses two complementary triggers:

- OCum Ascended's `ocum_applied_cum` custom ModEvent keeps the existing bounded
  low-latency snapshots after a cum event;
- OStim's reliable thread-0 `ostim_start` / `ostim_end` events arm a 0.5-second
  scene poll for the true local player. This fallback is necessary because the
  0.31.4 multiplayer test showed that `ocum_applied_cum` can fail to produce a
  live synchronization callback even though OCum has already changed the local
  scene state.

Every live poll:

- sends `OVR|ocum|CumOverlays`, asking Core to capture all RaceMenu
  Face/Body/Hands/Feet slots whose current texture contains `CumOverlays` and to
  refresh that same geometry locally;
- reads `OActor.IsObjectEquipped(..., "ocumvagmesh")` and
  `OActor.IsObjectEquipped(..., "ocumanmesh")` as the primary mesh source;
- ORs those values with the validated OCum F37/F3B armor equipped state as a
  fallback;
- sends the actual `OBJ|ocum|...` state to the remote client.

The OStim equip-object query is essential: an OCum mesh can be visibly active
while ordinary Skyrim inventory `IsWorn()` still reports its backing armor as
not worn.

## Build

0.31.5 changes this Papyrus source, so recompile it before building the FOMOD:

```powershell
.\optional\OCumIntegration\compile-ocum-integration.ps1 `
  -SkyrimDir "C:\Games\Steam\steamapps\common\Skyrim Special Edition"
```

Then run `build-fomod.ps1`. The FOMOD build intentionally refuses to package a
PEX older than this source.
