# Roadmap: PalVerify

## Phase 1 - Verified Core

**Goal:** Prove the platform policy and verification lifecycle outside Palworld.

**Requirements:** PLAT-01, PLAT-02, PLAT-03, PLAT-04, SESS-01, SESS-02,
SESS-03, SESS-04, SESS-05, SESS-06, SAFE-01, SAFE-03, SAFE-04

**Success criteria:**

1. Tests prove WinGDK and Mac require verification while exact Xbox console
   models and PS5 are exempt.
2. Unknown values fail closed.
3. Initial verification and heartbeat deadlines produce stable reason codes.
4. All tests pass under MSVC with no warnings.

## Phase 2 - Server Observation Adapter

**Goal:** Load on the Windows dedicated server and safely observe remote
platform values without enforcing kicks.

**Requirements:** PLAT-05, RUNT-01, RUNT-02

**Success criteria:**

1. UE4SS loads the server probe on the target Palworld build.
2. Startup compatibility checks identify required classes/functions.
3. Real test accounts produce distinct observations for Steam, WinGDK, Xbox
   console, and PS5.
4. Adapter mismatch keeps enforcement disabled.

## Phase 3 - Windows Enforcement

**Goal:** Require PalVerify on Steam Windows and WinGDK while preserving console
crossplay.

**Requirements:** RUNT-03, RUNT-04, SAFE-02

**Success criteria:**

1. Steam and WinGDK clients without PalVerify are kicked after the grace period.
2. Verified Windows clients remain connected through recurring heartbeats.
3. Xbox console and PS5 sessions remain exempt based only on server data.
4. Evidence logs identify the player, platform, version, and reason without
   uploading private inventories.

## Phase 4 - Mac Verification

**Goal:** Require and support PalVerify on Mac rather than exempting the
platform.

**Requirements:** RUNT-05

**Success criteria:**

1. A supported Mac client runtime can answer the same challenge protocol.
2. Mac clients without PalVerify fail closed.
3. Mac proof/version behavior passes the same conformance tests as Windows.

## Phase 5 - Detection Expansion

**Goal:** Add calibrated client integrity signals and server-authoritative
behavior detectors.

**Requirements:** v2 requirements promoted after Phase 3 calibration

**Success criteria:**

1. Client signals remain privacy-minimized and cannot independently permanent
   ban.
2. Server stamina and movement detectors use independent budgets and sustained
   evidence.
3. Shadow-mode telemetry is reviewed before enforcement thresholds are enabled.
