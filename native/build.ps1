param(
    [ValidateSet('x64', 'arm64')][string]$Architecture = 'x64',
    [switch]$SkipTests
)

$ErrorActionPreference = 'Stop'
$Architecture = $Architecture.ToLowerInvariant()
$root = Split-Path $PSScriptRoot -Parent
$output = Join-Path $root "artifacts\GlowTray\$Architecture"
$objects = Join-Path $root "artifacts\native-obj\$Architecture"
$hostArchitecture = [Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString().ToLowerInvariant()
if ($hostArchitecture -notin @('x64', 'arm64')) { throw 'A 64-bit Windows build host is required.' }
if ($Architecture -eq 'arm64' -and $hostArchitecture -ne 'arm64' -and -not $SkipTests) {
    throw 'ARM64 tests require an ARM64 Windows host. Use -SkipTests when cross-compiling.'
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw 'Visual Studio C++ Build Tools and a Windows SDK are required.' }
$component = if ($Architecture -eq 'arm64') { 'ARM64' } else { 'x86.x64' }
$installation = & $vswhere -latest -products '*' -requires "Microsoft.VisualStudio.Component.VC.Tools.$component" -property installationPath
if (-not $installation) { throw "MSVC $Architecture C++ Build Tools and a Windows SDK are required." }
$environment = Join-Path $installation 'VC\Auxiliary\Build\vcvarsall.bat'
$compilerArchitecture = if ($hostArchitecture -eq $Architecture) { $Architecture } else { "${hostArchitecture}_$Architecture" }
$null = New-Item -ItemType Directory -Force $output, $objects

Add-Type -AssemblyName System.Drawing
$bitmap = [Drawing.Bitmap]::new(32, 32)
$graphics = [Drawing.Graphics]::FromImage($bitmap)
$graphics.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::AntiAlias
$pen = [Drawing.Pen]::new([Drawing.Color]::FromArgb(135, 230, 188), 6)
$graphics.DrawEllipse($pen, 6, 6, 20, 20)
$handle = $bitmap.GetHicon()
$icon = [Drawing.Icon]::FromHandle($handle)
$stream = [IO.File]::Create((Join-Path $objects 'GlowTray.ico'))
try { $icon.Save($stream) }
finally {
    $stream.Dispose()
    $icon.Dispose()
    $pen.Dispose()
    $graphics.Dispose()
    $bitmap.Dispose()
    Add-Type -TypeDefinition 'public static class GlowTrayBuildIcons { [System.Runtime.InteropServices.DllImport("user32.dll")] public static extern bool DestroyIcon(System.IntPtr h); }'
    $null = [GlowTrayBuildIcons]::DestroyIcon($handle)
}

$flags = '/nologo /std:c++20 /EHsc /permissive- /W4 /WX /external:W0 /O1 /GL /Gy /Gw /MT /utf-8 /guard:cf /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_WIN32_WINNT=0x0A00'
$libraries = 'user32.lib gdi32.lib comctl32.lib shell32.lib advapi32.lib ole32.lib runtimeobject.lib windowsapp.lib winhttp.lib ws2_32.lib iphlpapi.lib dnsapi.lib dwmapi.lib msimg32.lib'
$link = "/nologo /LTCG /OPT:REF,ICF /DYNAMICBASE /NXCOMPAT /GUARD:CF /MACHINE:$Architecture"
if ($Architecture -eq 'x64') { $link += ' /CETCOMPAT' }
$commands = @(
    "set `"PATH=$(Split-Path $vswhere -Parent);%PATH%`"",
    "call `"$environment`" $compilerArchitecture >nul",
    "cd /d `"$PSScriptRoot`"",
    "rc /nologo /i `"$objects`" /fo `"$objects\GlowTray.res`" GlowTray.rc",
    "cl $flags /c LightCore.cpp /Fo`"$objects\LightCore.obj`"",
    "cl $flags /c GlowTray.cpp /Fo`"$objects\GlowTray.obj`"",
    "link $link /SUBSYSTEM:WINDOWS /OUT:`"$output\GlowTray.exe`" `"$objects\GlowTray.obj`" `"$objects\LightCore.obj`" `"$objects\GlowTray.res`" $libraries"
)
if (-not $SkipTests) {
    $commands += "cl $flags /c CoreTests.cpp /Fo`"$objects\CoreTests.obj`""
    $commands += "link $link /SUBSYSTEM:CONSOLE /OUT:`"$objects\GlowTray.Tests.exe`" `"$objects\CoreTests.obj`" `"$objects\LightCore.obj`" $libraries"
}
& $env:ComSpec /d /c ($commands -join ' && ')
if ($LASTEXITCODE -ne 0) { throw "Native build failed (exit $LASTEXITCODE)." }
if (-not $SkipTests) {
    & "$objects\GlowTray.Tests.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Native regression tests failed.' }
}
$executable = Get-Item "$output\GlowTray.exe"
Write-Output ("Built {0}: {1:N0} bytes ({2:N0} KiB)" -f $executable.FullName, $executable.Length, ($executable.Length / 1KB))
