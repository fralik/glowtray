param([string]$Executable = "$PSScriptRoot\..\artifacts\GlowTray\GlowTray.exe")

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class NativeLightUi {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string name);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr hwnd, int id);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr hwnd, uint msg, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hwnd, uint msg, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr hwnd, StringBuilder text, int max);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hwnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr dc, uint flags);
}
'@

$Executable = (Resolve-Path $Executable).Path
$testExe = (Resolve-Path "$PSScriptRoot\..\artifacts\native-obj\GlowTray.Tests.exe").Path
if (Get-Process -Name GlowTray -ErrorAction SilentlyContinue) {
    throw 'Close GlowTray before the isolated UI test.'
}
$settings = Join-Path $env:LOCALAPPDATA 'GlowTray\settings.json'
$backup = if (Test-Path $settings) { [IO.File]::ReadAllBytes($settings) } else { $null }
$startupPath = 'Software\Microsoft\Windows\CurrentVersion\Run'
$startupNames = @('GlowTray', 'RingLightNative')
$startupBackup = @{}
$startupKey = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($startupPath)
try {
    foreach ($name in $startupNames) {
        if ($null -ne $startupKey -and $startupKey.GetValueNames() -contains $name) {
            $startupBackup[$name] = @{
                Value = $startupKey.GetValue($name, $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
                Kind = $startupKey.GetValueKind($name)
            }
        }
    }
}
finally { if ($null -ne $startupKey) { $startupKey.Dispose() } }
$server = $null
$app = $null
$exitCode = 0

function Wait-Until([scriptblock]$Predicate, [string]$Message, [int]$Seconds = 15) {
    $deadline = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $deadline) {
        if (& $Predicate) { return }
        Start-Sleep -Milliseconds 100
    }
    throw $Message
}
function State { return (Invoke-RestMethod 'http://127.0.0.1:19124/state' -NoProxy).lights[0] }
function Text([IntPtr]$Window) {
    $buffer = [Text.StringBuilder]::new(2048)
    $null = [NativeLightUi]::GetWindowText($Window, $buffer, $buffer.Capacity)
    return $buffer.ToString()
}
function Set-Slider([IntPtr]$Slider, [int]$Value) {
    # Exercise the native keyboard path: TBM_SETPOS alone does not notify the parent.
    $previous = if ($Value -eq 0) { 1 } else { $Value - 1 }
    $key = if ($Value -eq 0) { 37 } else { 39 }
    $null = [NativeLightUi]::SendMessage($Slider, 1029, 1, $previous)
    $null = [NativeLightUi]::SendMessage($Slider, 256, $key, 0)
    $null = [NativeLightUi]::SendMessage($Slider, 257, $key, 0)
}
function Invoke-Button([IntPtr]$Button) {
    $null = [NativeLightUi]::SendMessage($Button, 245, 0, 0)
}
function Assert-SliderPaint([IntPtr]$Slider) {
    $rectangle = [NativeLightUi+RECT]::new()
    $null = [NativeLightUi]::GetClientRect($Slider, [ref]$rectangle)
    $bitmap = [Drawing.Bitmap]::new($rectangle.Right, $rectangle.Bottom)
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    $dc = $graphics.GetHdc()
    try { $painted = [NativeLightUi]::PrintWindow($Slider, $dc, 3) }
    finally { $graphics.ReleaseHdc($dc); $graphics.Dispose() }
    try {
        if (-not $painted) { throw 'Slider did not render.' }
        $trackPixels = 0
        $thumbPixels = 0
        for ($y = 0; $y -lt $bitmap.Height; $y += 2) {
            for ($x = 0; $x -lt $bitmap.Width; $x += 2) {
                $color = $bitmap.GetPixel($x, $y)
                if ($color.R -eq 241 -and $color.G -eq 245 -and $color.B -eq 249) { $thumbPixels++ }
                if ($color.R -ne 16 -or $color.G -ne 23 -or $color.B -ne 34) { $trackPixels++ }
            }
        }
        if ($thumbPixels -lt 10 -or $trackPixels -lt 100) { throw 'Slider track or thumb is not visible.' }
    }
    finally { $bitmap.Dispose() }
}

try {
    $server = Start-Process $testExe -ArgumentList '--serve' -PassThru -WindowStyle Hidden
    Write-Output "Fake native light PID: $($server.Id)"
    Wait-Until { try { $null = State; return $true } catch { return $false } } 'Native fake light failed to start.'
    & $testExe --integration
    if ($LASTEXITCODE -ne 0) { throw 'Native HTTP integration failed.' }
    $null = New-Item -ItemType Directory -Force (Split-Path $settings -Parent)
    [IO.File]::WriteAllText($settings,
        '{"Devices":[{"Address":"127.0.0.1","Port":19124,"Name":"C++ Test Ring Light","ServiceId":""}],"SelectedKey":"127.0.0.1:19124"}')
    $startupKey = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($startupPath)
    try {
        $startupKey.DeleteValue('GlowTray', $false)
        $startupKey.SetValue('RingLightNative', "`"$Executable`" --tray", [Microsoft.Win32.RegistryValueKind]::String)
    }
    finally { $startupKey.Dispose() }
    $app = Start-Process $Executable -ArgumentList '--add 127.0.0.1:19124' -PassThru
    Write-Output "GlowTray UI PID: $($app.Id)"
    Wait-Until { return [NativeLightUi]::FindWindow('GlowTray.Flyout', 'GlowTray') -ne [IntPtr]::Zero } 'GlowTray popup did not start.'
    $window = [NativeLightUi]::FindWindow('GlowTray.Flyout', 'GlowTray')
    [uint32]$ownerPid = 0
    $null = [NativeLightUi]::GetWindowThreadProcessId($window, [ref]$ownerPid)
    if ($ownerPid -ne $app.Id) { throw 'Found a different native app instance.' }
    $brightness = [NativeLightUi]::GetDlgItem($window, 1003)
    $temperature = [NativeLightUi]::GetDlgItem($window, 1004)
    $power = [NativeLightUi]::GetDlgItem($window, 1002)
    Wait-Until {
        return [NativeLightUi]::IsWindowEnabled($brightness) -and
            [NativeLightUi]::SendMessage($brightness, 1024, 0, 0).ToInt32() -eq 37
    } 'Native fake light did not display its initial state.'
    $startupKey = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($startupPath)
    try {
        if ($startupKey.GetValue('GlowTray') -ne "`"$Executable`" --tray" -or
            $null -ne $startupKey.GetValue('RingLightNative')) {
            throw 'Startup preference did not migrate to GlowTray.'
        }
    }
    finally { $startupKey.Dispose() }
    $null = [NativeLightUi]::PostMessage($window, 32771, 0, 0)
    Wait-Until { return [NativeLightUi]::IsWindowVisible($window) } 'Popup did not open.'
    Assert-SliderPaint $brightness
    Assert-SliderPaint $temperature
    Set-Slider $brightness 64
    Wait-Until { return (State).brightness -eq 64 } 'Native brightness slider failed.'
    if ((State).on -ne 0) { throw 'Changing brightness unexpectedly turned on the light.' }
    Set-Slider $temperature 54
    Wait-Until { return (State).temperature -eq 179 } 'Native temperature slider failed.'
    Invoke-Button $power
    Wait-Until { return (State).on -eq 1 } 'Native power button failed.'
    Invoke-Button ([NativeLightUi]::GetDlgItem($window, 1006))
    Wait-Until { return (State).temperature -eq 222 } 'Neutral preset failed.'
    $null = Invoke-RestMethod 'http://127.0.0.1:19124/offline' -NoProxy
    Wait-Until { return -not [NativeLightUi]::IsWindowEnabled($brightness) } 'Disconnect was not detected.' 15
    $app.Refresh()
    if (-not $app.Responding) { throw 'Popup stopped responding while the light was offline.' }
    $null = Invoke-RestMethod 'http://127.0.0.1:19124/online' -NoProxy
    Wait-Until { return [NativeLightUi]::IsWindowEnabled($brightness) } 'Light did not reconnect.' 15
    $null = [NativeLightUi]::PostMessage($brightness, 256, 27, 0)
    Wait-Until { return -not [NativeLightUi]::IsWindowVisible($window) } 'Escape did not dismiss popup.'
    Start-Sleep -Milliseconds 300
    $null = [NativeLightUi]::PostMessage($window, 32770, 0, 1024)
    Wait-Until { return [NativeLightUi]::IsWindowVisible($window) } 'Tray click did not reopen popup.'
    $null = [NativeLightUi]::PostMessage($window, 32770, 0, 1024)
    Wait-Until { return -not [NativeLightUi]::IsWindowVisible($window) } 'Second tray click did not dismiss popup.'
    Start-Sleep -Milliseconds 300
    $null = [NativeLightUi]::PostMessage($window, 32770, 0, 1025)
    Wait-Until { return [NativeLightUi]::IsWindowVisible($window) } 'Keyboard tray activation failed.'
    $null = [NativeLightUi]::PostMessage($window, 6, 0, 0)
    Wait-Until { return -not [NativeLightUi]::IsWindowVisible($window) } 'Outside activation did not dismiss popup.'
    $second = Start-Process $Executable -ArgumentList '--tray' -PassThru
    if (-not $second.WaitForExit(4000)) { Stop-Process -Id $second.Id; throw 'Single-instance check failed.' }
    Wait-Until { return [NativeLightUi]::IsWindowVisible($window) } 'Second launch did not reopen existing popup.'
    $null = [NativeLightUi]::PostMessage($window, 273, 2007, 0)
    if (-not $app.WaitForExit(8000)) { throw 'Native app failed to exit cleanly.' }
    if ($app.ExitCode -ne 0) { throw "Native app exited with code $($app.ExitCode)." }
    Write-Output 'PASS: GlowTray branding, startup migration, native HTTP, keyboard sliders, power, presets, offline reconnection, Escape/outside dismissal, tray toggle, keyboard activation, single instance, and clean exit.'
}
catch {
    Write-Output $_.ScriptStackTrace
    if ($null -ne $app) {
        $window = [NativeLightUi]::FindWindow('GlowTray.Flyout', 'GlowTray')
        if ($window -ne [IntPtr]::Zero) {
            Write-Output "Native status: $(Text ([NativeLightUi]::GetDlgItem($window, 1010)))"
        }
    }
    Write-Error $_ -ErrorAction Continue
    $exitCode = 1
}
finally {
    if ($null -ne $app -and -not $app.HasExited) { Stop-Process -Id $app.Id }
    if ($null -ne $server) {
        try { $null = Invoke-RestMethod 'http://127.0.0.1:19124/shutdown' -NoProxy } catch { Write-Warning $_ }
        if (-not $server.WaitForExit(3000)) { Stop-Process -Id $server.Id }
    }
    try {
        if ($null -ne $backup) { [IO.File]::WriteAllBytes($settings, $backup) }
        elseif (Test-Path $settings) { Remove-Item -LiteralPath $settings }
    }
    finally {
        $startupKey = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($startupPath)
        try {
            foreach ($name in $startupNames) {
                if ($startupBackup.ContainsKey($name)) {
                    $startupKey.SetValue($name, $startupBackup[$name].Value, $startupBackup[$name].Kind)
                }
                else { $startupKey.DeleteValue($name, $false) }
            }
        }
        finally { $startupKey.Dispose() }
    }
}
exit $exitCode
