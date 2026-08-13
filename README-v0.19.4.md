# OStim Together v0.19.4

Two-player test fix for the remaining Player1-only proxy issues.

## Local STR proxy position guard

On the locally-owned OStim thread, the dynamic Skyrim Together player proxy
now uses OStim's settled actor pose as its temporary position authority. The
guard starts after a short scene-start grace period, runs only while that
specific local thread is alive, and is removed at STOP so normal STR ownership
resumes immediately outside the scene.

This replaces the incomplete event-only `StopTranslation()` strategy. That
strategy successfully cancelled OStim's queued translations, but Player1 logs
still showed the proxy moving around the stable authoritative pose as STR
interpolation updates arrived from Player2.

Expected diagnostic:

```text
OSTNET LOCAL STR PROXY GUARD armed ... owner=OStimUntilStop
OSTNET LOCAL STR PROXY GUARD ... corrected=... owner=OStimUntilStop
```

## Remote RaceMenu overlay live application

Received overlay properties are still persisted with `AddNodeOverride`, but
are now also sent directly to the proxy's existing third-person geometry with
`SetNodeProperty`. This bypasses the gap where SKEE reported a complete stored
override state while the live dynamic proxy nodes stayed visually blank.

The receiver also mirrors stored properties under the proxy's live sex when
it temporarily differs from the sender metadata, and repeats the live apply at
T120, T500, and T1200 after any deferred RaceMenu rebuild.

No `GetNodeProperty()` call is used.

The generic addon object path and OCum vaginal/anal mesh synchronization are
unchanged from v0.19.3.
