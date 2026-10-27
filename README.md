# RingLight

## Native C++ / Win32 app

Run **`artifacts\RingLight.Native\RingLight.Native.exe`**.
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

Native settings and rotating logs are in `%LOCALAPPDATA%\RingLightNative`.
On first run, compatible saved addresses are imported from the C# app if available.
The old app's files, settings, and startup entry are never modified.

### Build and check the native app

Requires MSVC x64 C++ Build Tools, a Windows SDK, and PowerShell on Windows.
No packages or third-party libraries are downloaded.

```powershell
pwsh -File native\build.ps1
pwsh -File native\UiSmoke.ps1
```

The build produces the release executable and runs 39 protocol/settings regression checks.
The isolated UI check uses a native simulated light on loopback, restores native settings,
and exercises controls, disconnect/reconnect, tray activation/dismissal, single-instance
behaviour, and clean exit. Close the native app before running it.

Optional read-only real-device checks:

```powershell
artifacts\native-obj\RingLight.Tests.exe --live <address>
artifacts\native-obj\RingLight.Tests.exe --discover
```

Adding `--verify-write` after `--live <address>` re-applies the light's existing brightness
and verifies that its full state remains unchanged.

### GitHub Actions

Every push to `master` runs `.github\workflows\build-native.yml` on a Windows runner.
It builds the native x64 release with `native\build.ps1`, runs the native regression
checks, and uploads `RingLight.Native.exe` as the **RingLight-Native-windows-x64** artifact.
Download the artifact from the completed workflow run. UI smoke checks remain local
because they require an interactive Windows desktop.

The Git repository tracks only the native app, its build/test scripts, and documentation.
Generated binaries and the historical C# workspace are ignored. A local Git commit alone
does not run Actions; the commit must be pushed to a GitHub repository.

## Historical C# / WPF app

The original C# implementation is preserved locally in `src` and `tests`, with its existing
executable at `artifacts\RingLight\RingLight.exe`. These historical files are not tracked
in the native-only Git repository. The following instructions apply to that local copy,
not to a fresh clone. It is independent of the native app.

A native Windows tray controller for Elgato Ring Light and compatible white Key Lights.
It communicates directly with lights on your local network; Elgato Control Center, Bonjour,
an account, and cloud services are not required.

### Run the historical app

Open `artifacts\RingLight\RingLight.exe`. The portable, self-contained x64 build includes
the .NET runtime. No installer or administrator access is required.

Lights are discovered automatically on startup and every 45 seconds using the Elgato
`_elg._tcp.local.` mDNS service on your active network adapters. **Discover lights**
runs another search. If mDNS returns no devices, discovery checks port 9123 on the
directly attached Wi-Fi/Ethernet subnet, capped to the local /24 (at most 254 addresses
per adapter; no VPN or internet scanning). Devices are verified against the white-light
HTTP API before being added. Saved addresses reconnect independently of discovery.
If Windows asks, allow the app on your home network. **Add by IP address** remains
available when network client isolation prevents automatic discovery.

Choose a light to adjust power, brightness, and colour temperature (2900–7000 K).
Slider changes are debounced, combined, and sent serially. Only changed settings are sent;
adjusting a slider never turns a light on unexpectedly. Each write is followed by a
read-back. Errors are shown explicitly, and disconnected lights reconnect automatically.

Closing the window keeps the app in the system tray. Double-click its ring icon to reopen,
or right-click to toggle the selected light, discover devices, open the data folder, or exit.
**Start with Windows** is opt-in and starts minimized to the tray.

Settings and rotating error logs are stored in `%LOCALAPPDATA%\RingLight`.
Optional first-run argument: `RingLight.exe --add <address>`.
`RingLight.exe --tray` starts with the window hidden.
Run one copy at a time. If moving the executable, toggle **Start with Windows** off and on
to update its startup path.

This app controls lights that have already joined Wi-Fi. It does not provision/reset
lights, update firmware, control RGB Light Strips, or implement the newer MK.2 protocol.
It is an independent app, not an Elgato product.

### Build the historical app

Requires Windows and a .NET 8 or newer SDK.

```powershell
dotnet build src\RingLight.Desktop\RingLight.Desktop.csproj -c Release
dotnet run --project tests\RingLight.Tests -c Release
dotnet publish src\RingLight.Desktop\RingLight.Desktop.csproj -c Release -o artifacts\RingLight
```

Optional real-device checks are read-only:

```powershell
dotnet run --project tests\RingLight.Tests -c Release -- --live <address> --discover
```

The protocol/settings regression harness needs no test-framework dependency.
Discovery uses the MIT-licensed [Zeroconf](https://github.com/novotnyllc/Zeroconf) library.

An isolated native UI check uses a simulated light and restores saved settings afterward.
Close RingLight first, then run `pwsh -File tests\UiSmoke.ps1` after building and publishing.
It exercises both sliders, the accessible power switch, and close-to-tray without changing
any real light. `--live <address> --verify-write` additionally re-applies the real light's
existing brightness and confirms its full state is unchanged.
