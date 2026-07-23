# PalVerify

## What This Is

PalVerify is a paired client/server anti-cheat mod for a Windows Palworld
dedicated server. It requires supported PC and Mac players to prove that a
current PalVerify client is running continuously, while real Xbox consoles and
PlayStation 5 clients are exempt because they cannot load the same client mod.

The first release establishes trustworthy platform policy, a connection-bound
challenge, version enforcement, heartbeat timeouts, structured evidence, and a
server kick decision. Later releases add conservative server-authoritative
behavior detectors and supplementary client integrity signals.

## Core Value

A PC or Mac player cannot remain on the server without a current, continuously
verified PalVerify session, while legitimate Xbox console and PS5 players remain
able to join through crossplay.

## Requirements

### Validated

(None yet - ship to validate)

### Active

- [ ] Distinguish Steam Windows, WinGDK/Game Pass PC, Mac, PS5, and individual
      Xbox console families using server-owned session data.
- [ ] Exempt only confirmed Xbox console hardware and PS5.
- [ ] Require Steam Windows, WinGDK/Game Pass PC, Mac, and unknown platforms to
      complete PalVerify verification.
- [ ] Challenge required clients and reject missing, stale, invalid, or
      unsupported client versions.
- [ ] Kick required clients after verification or heartbeat grace periods
      expire.
- [ ] Bind every decision to a server-observed player identity and platform.
- [ ] Emit structured, privacy-minimized evidence for every allow, exempt,
      reject, timeout, and integrity decision.
- [ ] Package client and server adapters for the current Palworld mod loader and
      UE4SS layout where supported.
- [ ] Add server-authoritative behavior checks without trusting client-reported
      gameplay values.

### Out of Scope

- Kernel drivers - unacceptable operational and privacy cost for the first
  release.
- EAC/Vanguard-equivalent guarantees - unavailable to a community mod without
  first-party integration.
- Permanent bans from one client signal - process and integrity signals are
  bypassable and can false-positive.
- Whole-disk scanning or uploading process lists - unnecessary collection of
  private user data.
- Exempting Microsoft Store/Game Pass PC as Xbox - that would create a direct
  bypass.

## Context

- The dedicated server runs Windows and allows crossplay.
- Only real Xbox console hardware and PS5 are exempt.
- Steam Windows, WinGDK/Game Pass PC, and Mac must run PalVerify.
- Community SDK/header evidence exposes supported-platform values that separate
  Windows, WinGDK, Mac, PS5, and Xbox hardware models. The exact per-connection
  server access path must be validated against the deployed Palworld build.
- Palworld's public REST player schema does not expose a documented platform
  field, so REST-only enforcement is insufficient for the final policy.
- Existing local PalAntiCheat work demonstrates a conservative UE4SS server
  detector pattern and supplies future stamina-detector research.

## Constraints

- **Authority**: Platform classification must come from the authenticated server
  session, never a client-supplied string.
- **Compatibility**: The production server adapter targets Windows Palworld
  dedicated server and the Palworld-specific UE4SS build.
- **Crossplay**: PS5 and confirmed Xbox console models bypass client
  verification; WinGDK does not.
- **Mac**: Mac is required to verify. Until a compatible Mac client runtime is
  proven and shipped, Mac connections fail closed rather than being exempted.
- **Security**: Nonces and session proofs prevent network replay, but cannot make
  a fully compromised client trustworthy.
- **Safety**: Unknown platform values fail closed; runtime adapter mismatches
  disable unsafe enforcement and produce explicit compatibility errors.
- **Privacy**: Client inspection is limited to running processes/modules and
  reports rule identifiers instead of full inventories.
- **Release**: Automatic permanent bans are disabled by default.

## Key Decisions

| Decision | Rationale | Outcome |
|----------|-----------|---------|
| Paired client/server design | Client-only reporting can be omitted or forged without server enforcement | Pending |
| Exempt exact Xbox console models and PS5 only | Consoles cannot load the client mod, while generic Xbox identity can include WinGDK PC | Pending |
| Require Mac verification | User explicitly requires Mac instead of blocking or exempting it | Pending |
| Unknown platforms fail closed | New or unmapped platform values must not become a bypass | Pending |
| Keep platform policy independent of SDK numeric values | Game updates can reorder or replace generated enum values | Pending |
| Test pure enforcement core outside UE4SS | Deterministic policy and timeout behavior must be regression tested | Pending |
| Server-authoritative gameplay detection | Client gameplay state is attacker-controlled | Pending |
| No automatic permanent bans in v1 | Integrity signals are probabilistic and need calibration | Pending |

## Evolution

This document evolves at phase transitions and milestone boundaries.

After each phase transition:

1. Move invalidated requirements to Out of Scope with the reason.
2. Move shipped and verified requirements to Validated.
3. Add newly discovered requirements to Active.
4. Record consequential decisions in Key Decisions.
5. Update the product description if implementation reality changes.

After each milestone, review the core value, requirements, constraints, and
out-of-scope boundaries in full.

---
*Last updated: 2026-07-24 after project initialization*
