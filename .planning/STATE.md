# Project State

## Project Reference

See: `.planning/PROJECT.md` (updated 2026-07-24)

**Core value:** Required PC and Mac clients cannot remain connected without a
current PalVerify session, while real Xbox consoles and PS5 retain crossplay.

**Current focus:** Phase 1 - Verified Core

## Status

- Project initialized.
- Local Visual Studio 2022 toolchain discovered.
- Community SDK evidence suggests WinGDK and Xbox hardware can be separated.
- Runtime per-connection platform access remains unverified.
- Pure core TDD cycle is next.

## Decisions

- Exempt exact Xbox console models and PS5 only.
- Require Steam Windows, WinGDK/Game Pass PC, and Mac.
- Unknown platforms fail closed.
- Keep runtime adapter values outside the pure core.
- Start enforcement in observation mode.
