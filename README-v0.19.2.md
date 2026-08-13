# OStim Together v0.19.2

This test build keeps the v0.19.1 core and optional OCum architecture and
adds the fixes identified during the two-player in-game test.

- Synchronizes OStim speed changes from the authoritative player to the
  mirrored scene.
- Releases OStim's translation ownership after speed replays as well as after
  START and NODE events.
- Applies captured RaceMenu overlay overrides directly to their live 3D nodes
  on both the local player and the remote Skyrim Together proxy.
- Detects OCum's vaginal and anal meshes from their actual equipped armor
  forms and sends both states on every bounded probe.
- Reapplies cached remote addon overlays and equip objects after OStim's scene
  cleanup so persistent OCum visuals survive scene exit.

Build and installation details remain documented in
[README-v0.19.1.md](README-v0.19.1.md).
