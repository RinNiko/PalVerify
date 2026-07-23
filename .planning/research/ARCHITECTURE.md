# Architecture Research

## Components

1. `palverify_core`
   - Stable platform model and verification policy.
   - Session state machine, deadlines, version policy, and kick reasons.
   - No Unreal, UE4SS, network, or operating-system dependencies.
2. `PalVerifyServer`
   - Reads authenticated connection identity and platform.
   - Maps Palworld runtime values to the stable platform model.
   - Issues challenges, receives proofs/heartbeats, and executes kick decisions.
3. `PalVerifyClient`
   - Receives server challenges.
   - Produces versioned proof and recurring heartbeat.
   - Later gathers minimized integrity signals.
4. Packaging
   - Client and server install rules in the official Palworld package layout.

## Trust Boundaries

- Server session identity and platform are authoritative inputs.
- Client proof is evidence of protocol participation, not proof that the entire
  machine is clean.
- Server gameplay observations are preferred over client gameplay telemetry.
- An adapter must never convert unknown runtime values into an exempt platform.

## Platform Policy

- Exempt: exact `PS5Base`, `PS5Trinity`, and Xbox console hardware families.
- Required: Windows/Steam, WinGDK/Game Pass PC, Linux, Mac, generic PC, and
  Unknown.

## Runtime Spike

The first runtime spike must answer:

1. Which server object owns the remote player's supported-platform value?
2. Is it populated before or immediately after `PostLogin`?
3. Does WinGDK remain distinct from Xbox console hardware in a real crossplay
   session?
4. Can a connection-bound RPC or replicated PalVerify actor carry the challenge
   without accepting unauthenticated external HTTP claims?
