# Requirements: PalVerify

**Defined:** 2026-07-24  
**Core Value:** A PC or Mac player cannot remain without a current PalVerify
session, while real Xbox consoles and PS5 remain able to join.

## v1 Requirements

### Platform Policy

- [ ] **PLAT-01**: Server can map a remote connection to a stable internal
  platform without trusting client-supplied platform data.
- [ ] **PLAT-02**: Confirmed Xbox console models and PS5 bypass client
  verification.
- [ ] **PLAT-03**: Steam Windows, WinGDK/Game Pass PC, Mac, generic PC, and
  Unknown require verification.
- [ ] **PLAT-04**: Unknown or unmapped runtime platform values never become
  exempt.
- [ ] **PLAT-05**: Server can run platform discovery in observation-only mode
  before enforcement is enabled.

### Session Verification

- [ ] **SESS-01**: Required clients receive a connection-bound challenge.
- [ ] **SESS-02**: Server accepts only configured client protocol versions.
- [ ] **SESS-03**: Required clients that miss the initial verification deadline
  receive a deterministic kick decision.
- [ ] **SESS-04**: Verified clients must refresh their heartbeat within a
  configured interval.
- [ ] **SESS-05**: Missing, stale, invalid, or replayed session proof produces a
  deterministic rejection reason.
- [ ] **SESS-06**: Disconnecting removes all session state and challenge
  material.

### Runtime Integration

- [ ] **RUNT-01**: Windows dedicated server adapter loads through the supported
  Palworld-specific UE4SS native lifecycle.
- [ ] **RUNT-02**: Runtime compatibility failures disable kick enforcement and
  emit an explicit error.
- [ ] **RUNT-03**: Server kick execution is bound to the authenticated player
  identity and occurs on a safe game thread.
- [ ] **RUNT-04**: Windows/Steam and WinGDK client adapters can answer
  challenges and send heartbeats.
- [ ] **RUNT-05**: Mac connections are required to verify and fail closed until
  a compatible Mac client adapter is shipped.

### Evidence and Safety

- [ ] **SAFE-01**: Every exemption, verification, rejection, timeout, and kick
  has a structured reason code.
- [ ] **SAFE-02**: Client integrity reports contain rule identifiers instead of
  complete process or file inventories.
- [ ] **SAFE-03**: Automatic permanent bans are disabled in v1.
- [ ] **SAFE-04**: All pure policy and session behaviors are covered by
  repeatable unit tests.

## v2 Requirements

### Client Integrity

- **INTG-01**: Windows client can detect configured running process and loaded
  module signatures.
- **INTG-02**: Client can verify its signed release manifest and own module hash.
- **INTG-03**: Server can combine multiple integrity signals under configurable
  kick thresholds.

### Behavior Detection

- **BEHV-01**: Server maintains an independent stamina budget.
- **BEHV-02**: Server detects impossible movement and teleport displacement.
- **BEHV-03**: Server detects impossible skill cooldown and damage rates.
- **BEHV-04**: Server detects impossible inventory transaction rates.

## Out of Scope

| Feature | Reason |
|---------|--------|
| Kernel driver | Excessive security, privacy, signing, and maintenance cost |
| Whole-disk scanning | Unnecessary and invasive |
| Trusting a client platform claim | Direct exemption bypass |
| Automatic permanent ban | Signals require calibration and human review |
| EAC-equivalent assurance | Requires first-party integration |

## Traceability

| Requirement | Phase | Status |
|-------------|-------|--------|
| PLAT-01 | Phase 1 | In Progress |
| PLAT-02 | Phase 1 | In Progress |
| PLAT-03 | Phase 1 | In Progress |
| PLAT-04 | Phase 1 | In Progress |
| PLAT-05 | Phase 2 | Pending |
| SESS-01 | Phase 1 | In Progress |
| SESS-02 | Phase 1 | In Progress |
| SESS-03 | Phase 1 | In Progress |
| SESS-04 | Phase 1 | In Progress |
| SESS-05 | Phase 1 | In Progress |
| SESS-06 | Phase 1 | In Progress |
| RUNT-01 | Phase 2 | Pending |
| RUNT-02 | Phase 2 | Pending |
| RUNT-03 | Phase 3 | Pending |
| RUNT-04 | Phase 3 | Pending |
| RUNT-05 | Phase 4 | Pending |
| SAFE-01 | Phase 1 | In Progress |
| SAFE-02 | Phase 3 | Pending |
| SAFE-03 | Phase 1 | In Progress |
| SAFE-04 | Phase 1 | In Progress |

**Coverage:**

- v1 requirements: 19 total
- Mapped to phases: 19
- Unmapped: 0

---
*Requirements defined: 2026-07-24*  
*Last updated: 2026-07-24 after roadmap creation*
