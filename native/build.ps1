param([switch]$SkipTests)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$output = Join-Path $root 'artifacts\RingLight.Native'
$objects = Join-Path $root 'artifacts\native-obj'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'MSVC C++ Build Tools and Windows SDK are required.' }
$environment = Join-Path $installation 'VC\Auxiliary\Build\vcvars64.bat'
$null = New-Item -ItemType Directory -Force $output, $objects

Add-Type -AssemblyName System.Drawing
$bitmap = [Drawing.Bitmap]::new(32, 32)
$graphics = [Drawing.Graphics]::FromImage($bitmap)
$graphics.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::AntiAlias
$pen = [Drawing.Pen]::new([Drawing.Color]::FromArgb(135, 230, 188), 6)
$graphics.DrawEllipse($pen, 6, 6, 20, 20)
$handle = $bitmap.GetHicon()
$icon = [Drawing.Icon]::FromHandle($handle)
$stream = [IO.File]::Create((Join-Path $objects 'RingLight.ico'))
try { $icon.Save($stream) }
finally {
    $stream.Dispose()
    $icon.Dispose()
    $pen.Dispose()
    $graphics.Dispose()
    $bitmap.Dispose()
    Add-Type -TypeDefinition 'public static class RingLightBuildIcons { [System.Runtime.InteropServices.DllImport("user32.dll")] public static extern bool DestroyIcon(System.IntPtr h); }'
    $null = [RingLightBuildIcons]::DestroyIcon($handle)
}

$flags = '/nologo /std:c++20 /EHsc /permissive- /W4 /WX /external:W0 /O1 /GL /Gy /Gw /MT /utf-8 /guard:cf /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_WIN32_WINNT=0x0A00'
$libraries = 'user32.lib gdi32.lib comctl32.lib shell32.lib advapi32.lib ole32.lib runtimeobject.lib windowsapp.lib winhttp.lib ws2_32.lib iphlpapi.lib dnsapi.lib dwmapi.lib msimg32.lib'
$link = "/nologo /LTCG /OPT:REF,ICF /DYNAMICBASE /NXCOMPAT /CETCOMPAT /GUARD:CF"
$commands = @(
    "set `"PATH=$(Split-Path $vswhere -Parent);%PATH%`"",
    "call `"$environment`" >nul",
    "cd /d `"$PSScriptRoot`"",
    "rc /nologo /i `"$objects`" /fo `"$objects\RingLight.res`" RingLight.rc",
    "cl $flags /c LightCore.cpp /Fo`"$objects\LightCore.obj`"",
    "cl $flags /c RingLight.cpp /Fo`"$objects\RingLight.obj`"",
    "link $link /SUBSYSTEM:WINDOWS /OUT:`"$output\RingLight.Native.exe`" `"$objects\RingLight.obj`" `"$objects\LightCore.obj`" `"$objects\RingLight.res`" $libraries"
)
if (-not $SkipTests) {
    $commands += "cl $flags /c CoreTests.cpp /Fo`"$objects\CoreTests.obj`""
    $commands += "link $link /SUBSYSTEM:CONSOLE /OUT:`"$objects\RingLight.Tests.exe`" `"$objects\CoreTests.obj`" `"$objects\LightCore.obj`" $libraries"
}
& $env:ComSpec /d /c ($commands -join ' && ')
if ($LASTEXITCODE -ne 0) { throw "Native build failed (exit $LASTEXITCODE)." }
if (-not $SkipTests) {
    & "$objects\RingLight.Tests.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Native regression tests failed.' }
}
$executable = Get-Item "$output\RingLight.Native.exe"
Write-Output ("Built {0}: {1:N0} bytes ({2:N0} KiB)" -f $executable.FullName, $executable.Length, ($executable.Length / 1KB))
