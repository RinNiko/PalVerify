# Project State

## Project Reference

See: `.planning/PROJECT.md` (updated 2026-07-24)

**Core value:** Required PC and Mac clients cannot remain connected without a
current PalVerify session, while real Xbox consoles and PS5 retain crossplay.

**Current focus:** Phase 2 - Server Observation Adapter

## Status

- Project initialized and committed.
- Pure C++ platform/session core implemented through observed RED-GREEN TDD.
- Release build passes 13 C++ tests with MSVC warnings treated as errors.
- Windows process scanning compiles with exact-name WeMod and Cheat Engine rules
  that return only minimized rule identifiers.
- Lua symbol filter passes 4 tests and the observation probe parses cleanly.
- Official package metadata resolves every target and thumbnail.
- Palworld zDev runtime artifact was downloaded and SHA-256 verified.
- Native UE4SS source build requires GitHub access to Epic-licensed UEPseudo;
  the lifecycle source scaffold is present but cannot be compiled in this
  environment without that access.
- Runtime per-connection platform access remains unverified on a live server.

## Decisions

- Exempt exact Xbox console models and PS5 only.
- Require Steam Windows, WinGDK/Game Pass PC, Linux, and Mac.
- Unknown platforms fail closed.
- Keep runtime adapter values outside the pure core.
- Start enforcement in observation mode.

## Next Evidence Needed

1. Install the observation-only server package on the target server.
2. Capture `class_candidates` lines from `UE4SS.log`.
3. Determine whether a per-player platform field/function is exposed.
4. Test real Steam, WinGDK, Linux, Mac, Xbox console, and PS5 sessions before
   enabling any kick action.
