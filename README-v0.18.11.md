# OStim Together v0.18.11 — Furniture SELF pre-anchor

Based on v0.18.10a.

## Paired-log diagnosis

v0.18.10a successfully synchronizes the exact furniture reference.

Player1 can detect furniture hundreds of Skyrim units away and transmits the
exact placed reference.

Player2 resolves that same reference with distance=0 and calls:

    builder->setFurniture(localFurniture)

The remaining distance-dependent failure is caused by this old behavior:

    PREANCHOR SELF skipped reason=furniture

So the local PlayerCharacter can still be hundreds of units away when the
asynchronous mirror thread starts.

## OStim 7.4c behavior

For furniture threads OStim builds its center from:

    furniture.getPosition()

then applies its furniture offset and rotation.

Actor placement is later queued through:

    alignActor()
      -> lockAtPosition()
      -> TranslateTo()

v0.18.11 removes the large initial translation from that path.

## New behavior

If the local SELF participates and exact furniture was resolved:

1. StopReferenceTranslation(SELF)
2. SetPosition(SELF, raw furniture reference position)
3. SetRotationZ(SELF, furniture rotation)
4. Update3DPosition / ForcePlayer3DToReference
5. builder->setFurniture(exact furniture)
6. builder->start()
7. OStim applies its own furniture offset / scale / final alignment

The staging position is deliberately the RAW furniture reference. The plugin
does not attempt to reproduce OStim's private furniture offset math.

Expected Player2 log:

    OSTNET PREANCHOR SELF FURNITURE
      actor=00000014
      furnitureRef=...
      distanceBefore=...

followed by:

    OSTNET MIRROR FURNITURE EXACT using ...

## Unchanged

- exact locked-furniture detection on Player1
- exact/coordinate furniture resolver on Player2
- no private OStim Thread ABI access
- initial animation replay
- one-shot NODE position sync
- STR ownership of sender proxy
- NPC equipment lock and outfit restore
- non-furniture scene pre-anchor logic

## Build

```powershell
$env:VCPKG_ROOT="C:\dev\vcpkg"
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass

Remove-Item -Recurse -Force .\build -ErrorAction SilentlyContinue
.\build-vortex.ps1
```

Output:

`OStimTogether-v0.18.11-Vortex.zip`
