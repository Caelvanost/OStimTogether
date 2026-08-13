# OStim Together v0.18.14 — STR proxy appearance guard

This revision keeps the v0.18.13 delayed Wall START work and changes only
appearance/outfit protection for Skyrim Together remote-player proxies.

## Problem

A remote STR player proxy is not `IsPlayerRef()` on the observing client.
Older OStimTogether builds therefore treated it as a normal NPC and ran
`DefaultOutfitGuard`, temporarily setting the proxy's dynamic TESNPC
`defaultOutfit` to null.  That dynamic base also carries/participates in the
remote player's visual identity, so touching it can disturb RaceMenu/NiOverride
appearance state (for example makeup/overlays) and other visual attachments.

## Fix

`DefaultOutfitGuard` now identifies a likely STR remote player proxy when both
the actor reference FormID and its TESNPC base FormID are runtime dynamic
`FFxxxxxx` forms.  Those actors are excluded from Capture/Protect/Release.

`EquipmentLock` is intentionally unchanged and still removes body/hand/feet
armor from the remote proxy while an OStim scene is active, preserving the
anti-redress behavior.

Expected diagnostic on the observing client:

    DefaultOutfitGuard SKIP STR proxy actor=FF...... base=FF......

A normal NPC should continue to log `DefaultOutfitGuard ON ...`.
