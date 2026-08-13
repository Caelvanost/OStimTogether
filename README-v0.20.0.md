# OStim Together v0.20.0

Compatibility update for OStim Standalone 7.5b while retaining support for
the previously tested OStim 7.4c runtime.

## OStim 7.5b graph compatibility

OStim Together reconstructs the authoritative scene center and actor poses
from OStim's public alignment API plus the per-actor offset stored in the
current graph node. OStim 7.5 added `GraphActor::singleSpeed` before the
expression fields, moving the internal actor offset from `0x64` to `0x6C`.

v0.20.0 keeps separate, read-only layouts for:

- OStim 7.4c / `OStim.dll` 7.4.0.3
- OStim 7.5b / `OStim.dll` 7.5.0.2

The loaded DLL version selects the matching layout before any scene callbacks
are registered. Unknown OStim DLL versions are rejected with a critical log
instead of reading an unverified memory offset and risking corrupted proxy
positions.

Expected startup diagnostics on the new OStim release:

```text
OStim runtime DLL version=7.5.0.2 graphLayout=OStim-7.5b
OStim Threads interface version=3
```

The OStim 7.5b public ThreadBuilder and Node ABI prefixes used by the project
remain compatible. Its five climax/end-setting form IDs are also unchanged
from 7.4c.

## Unchanged synchronization

The v0.19.6 STR proxy stability work and cum-mesh synchronization are
unchanged.

## OCum overlay materialization

The v0.19.6 logs proved that OCum's texture and visible alpha reached the
correct live `Body [OvlN]` geometry on both clients, but the geometry could
still carry OverlayFix's `Hidden` and `DisableSorting` flags. `SetAppCulled`
does not clear those flags.

v0.20.0 now mirrors OverlayFix's own visible-overlay repair on the exact OCum
overlay subtree: it clears `Hidden` and `DisableSorting`, enables
`AlwaysDraw`, and refreshes the lighting shader after applying the texture and
alpha.

Expected diagnostics include non-zero `hiddenCleared` or `alwaysDraw` on the
first application, followed by `visibleMaterials=1 texturedMaterials=1`.
