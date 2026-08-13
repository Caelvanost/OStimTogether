# OStim Together v0.20.1

Remote-connection branch based on v0.20.0's OStim Standalone 7.5b
compatibility and OCum overlay materialization fixes.

## Internet / distant connection

OStim Together now uses the same remote connection model as the companion
mods in this workspace:

- LAN discovery still works by UDP broadcast on port `27991`.
- Remote clients can keep `AutoDiscovery=1` and `AutoRemoteFromSTR=1`.
- When a remote client has connected to Skyrim Together Reborn by direct
  connect, OStim Together reads STR's saved host address and contacts that same
  host on UDP `27991`.
- `RemotePeers=` accepts comma/semicolon-separated IPv4 addresses or DNS names,
  with optional per-peer ports.
- Legacy `PeerHost` / `PeerPort` remains supported and is added to
  `RemotePeers`.

## Player1 host / relay profile

The FOMOD now has an Internet role step.

Use **Client / LAN / no Internet relay** on regular players. Use
**Player1 host / Internet relay - requires port forwarding** only on the
Skyrim Together host / Player1 machine.

For the relay host:

1. Forward UDP port `27991` from the router to the Player1 PC.
2. Allow UDP `27991` through Windows Firewall.
3. Install the FOMOD relay-host option on that PC only.

The relay host accepts client hello packets, learns each client's observed
source endpoint, and forwards gameplay packets between known peers. Remote
clients usually do not need their own port forward as long as they can send
outbound UDP to the host.

## Optional authentication

`SharedSecret=` can be set on every client to require HMAC-SHA256 on discovery
and gameplay packets. `AutoSharedSecretFromSTR=1` can reuse STR's saved
direct-connect password on clients and `STServer.ini`'s `sPassword` on the
relay host when available.

The default keeps authentication off for the most forgiving first remote test.

## Expected diagnostics

On remote clients:

```text
OSTNET STR auto remote configured address="host:10578" endpoint=x.x.x.x:27991
UDP transport started AUTO=1 RELAY=0 AUTH=0 client="..." port=27991 configuredPeers=1 ...
```

On the Player1 relay host:

```text
UDP transport started AUTO=0 RELAY=1 AUTH=0 client="..." port=27991 ...
OSTNET DISCOVERED peer="..." addr=x.x.x.x:yyyyy instance=...
OSTNET RELAY source=x.x.x.x:yyyyy peers=... sender="..."
```

The v0.20.0 OStim 7.5b ABI compatibility and OCum overlay rendering changes
are otherwise unchanged.
