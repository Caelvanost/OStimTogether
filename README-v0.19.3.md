# OStim Together v0.19.3

Hotfix for the scene-launch freeze introduced by v0.19.2.

OStim can invoke its speed listener while holding the internal thread lock.
v0.19.2 synchronously called `GetCurrentSpeed()` from that listener, which
could re-enter the same lock and freeze Skyrim before the scene START event.

v0.19.3 records the callback without calling the OStim ModAPI, then reads and
sends the speed from the next SKSE game task after the callback has returned.
Mirror-generated speed callbacks remain suppressed to prevent feedback loops.

All v0.19.2 speed, RaceMenu overlay, and OCum mesh synchronization changes are
retained.
