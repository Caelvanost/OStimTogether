# Optional OCum Ascended Integration

This component is intentionally outside the OStimTogether core.

## Source of truth

It listens to OCum Ascended's `ocum_applied_cum` custom ModEvent.  The local
true player is authoritative.  Remote STR proxies are ignored as senders.

After an OCum event it performs four bounded snapshots (0.15 / 0.50 / 1.25 /
2.50 seconds after the event):

- `OVR|ocum|CumOverlays` asks the Core to sync all RaceMenu Face/Body/Hands/Feet
  slots whose current texture contains `CumOverlays`.
- for `vagina`, `OActor.IsObjectEquipped(..., "ocumvagmesh")` is sent as the
  actual object state.
- for `rectum`, `OActor.IsObjectEquipped(..., "ocumanmesh")` is sent as the
  actual object state.
- face/mouth/throat use overlays only; no fake facial mesh is created.

This fixes the v0.18.34 mistake where a vaginal decal could cause Player1 to
invent a vaginal mesh that Player2 did not actually have.
