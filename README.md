# GlowTray

A tiny native Windows tray controller for Elgato lights.

## Run

Run **`artifacts\GlowTray\GlowTray.exe`**.
The portable x64 release is approximately **400 KiB**, with a statically linked C++ runtime.
It uses Windows system DLLs only: no .NET runtime, browser engine, external DLLs, installer,
or administrator access is required. Windows 10 or later is required.

Single-click the ring icon in the system tray to open a compact control popup beside it.
Click again, press Escape, click outside, or use the popup's close button to hide it.
The app stays in the tray, with no taskbar button. The panel includes a light selector,
power, brightness, colour temperature, Warm/Neutral/Daylight presets, discovery,
and manual IP entry. Sliders also work with the keyboard. Tray keyboard activation
opens the panel as well.

Right-click the icon for **Start with Windows**, the data folder, forgetting the selected
light, and **Exit**. Startup is opt-in. `--tray` starts hidden; `--add <address>` connects
to and selects a specific light. A second launch opens the existing popup.

Network operations run in background threads. Slider edits are debounced and combined,
and writes take priority over polling. Only changed fields are sent, with an explicit
Content-Length for compatibility with Ring Light firmware. Successful writes are read
back from the light. Offline controls are disabled and reconnect automatically.

Discovery uses Windows mDNS for `_elg._tcp.local`, then checks Elgato's port 9123 on
directly attached Wi-Fi/Ethernet networks if multicast finds nothing. Large networks
are bounded to the local /24; VPN/tunnel interfaces are excluded. Candidates must pass
the white-light API check before being added. Saved lights reconnect independently.
Your light must already be connected to Wi-Fi; this does not provision lights or update
their firmware. White Ring Light/Key Light devices are supported, not RGB strips or MK.2.

Settings and rotating logs are in `%LOCALAPPDATA%\GlowTray`.
Saved light addresses and enabled startup preferences migrate automatically when upgrading.

## Build and check

Requires MSVC x64 C++ Build Tools, a Windows SDK, and PowerShell on Windows.
No packages or third-party libraries are downloaded.

```powershell
pwsh -File native\build.ps1
pwsh -File native\UiSmoke.ps1
```

The build produces `GlowTray.exe` and runs the protocol/settings regression checks.
The isolated UI check uses a native simulated light on loopback, restores native settings,
and exercises controls, disconnect/reconnect, tray activation/dismissal, single-instance
behaviour, startup migration, and clean exit. Close GlowTray before running it.

Optional read-only real-device checks:

```powershell
artifacts\native-obj\GlowTray.Tests.exe --live <address>
artifacts\native-obj\GlowTray.Tests.exe --discover
```

Adding `--verify-write` after `--live <address>` re-applies the light's existing brightness
and verifies that its full state remains unchanged.

## GitHub Actions

Every push to `master` runs `.github\workflows\build-native.yml` on a Windows runner.
It builds the native x64 release with `native\build.ps1`, runs the native regression
checks, and uploads `GlowTray.exe` as the **GlowTray-windows-x64** artifact.
Download the artifact from the completed workflow run. UI smoke checks remain local
because they require an interactive Windows desktop.

The Git repository tracks only the native app, its build/test scripts, and documentation.
Generated binaries are ignored. A local Git commit alone
does not run Actions; the commit must be pushed to a GitHub repository.
