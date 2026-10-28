# Changelog

## Unreleased

## 1.0.0

### Added

- Native C++/Win32 tray controller for Elgato Ring Light and compatible white Key Light devices.
- Tray-anchored popup with power, brightness, colour temperature, and Warm/Neutral/Daylight presets.
- Automatic light discovery through mDNS and bounded local-network probing, plus manual address entry.
- Multiple saved lights, automatic reconnection, background networking, and debounced control updates.
- Opt-in Windows startup, keyboard controls, and single-instance behaviour.
- Settings and rotating logs under `%LOCALAPPDATA%\GlowTray`, with migration from earlier app names.
- Portable x64 and ARM64 executables and per-user EXE installers that preserve settings and startup preferences.
- GitHub Actions builds, native regression tests, and installer lifecycle checks for both architectures.
- Local UI smoke checks using a simulated light.
- MIT license.
