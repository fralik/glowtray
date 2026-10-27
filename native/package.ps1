param(
    [ValidateSet('x64', 'arm64')][string]$Architecture = 'x64',
    [string]$Compiler
)

$ErrorActionPreference = 'Stop'
$Architecture = $Architecture.ToLowerInvariant()
$root = Split-Path $PSScriptRoot -Parent
$executable = (Get-Item (Join-Path $root "artifacts\GlowTray\$Architecture\GlowTray.exe")).FullName
$version = [Diagnostics.FileVersionInfo]::GetVersionInfo($executable).ProductVersion
if ($version -notmatch '^\d+\.\d+\.\d+(\.\d+)?$') { throw "Invalid GlowTray product version: $version" }

$reader = [IO.BinaryReader]::new([IO.File]::OpenRead($executable))
try {
    if ($reader.ReadUInt16() -ne 0x5A4D) { throw 'GlowTray.exe is not a PE executable.' }
    $reader.BaseStream.Position = 0x3C
    $offset = $reader.ReadInt32()
    if ($offset -lt 64 -or $offset -gt $reader.BaseStream.Length - 6) { throw 'Invalid PE header offset.' }
    $reader.BaseStream.Position = $offset
    if ($reader.ReadUInt32() -ne 0x4550) { throw 'Invalid PE signature.' }
    $machine = $reader.ReadUInt16()
    $expected = if ($Architecture -eq 'arm64') { 0xAA64 } else { 0x8664 }
    if ($machine -ne $expected) { throw "GlowTray.exe does not match the requested $Architecture installer." }
}
finally { $reader.Dispose() }

if (-not $Compiler) {
    $command = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if ($command) { $Compiler = $command.Source }
    else {
        $Compiler = @(
            "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
            "$env:ProgramFiles\Inno Setup 6\ISCC.exe",
            "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
        ) | Where-Object { Test-Path $_ } | Select-Object -First 1
    }
}
if (-not $Compiler) { throw 'Inno Setup 6.3 or later is required. Install it or pass -Compiler <path to ISCC.exe>.' }
$Compiler = (Get-Item $Compiler).FullName
$output = Join-Path $root "artifacts\installers\$Architecture"
$null = New-Item -ItemType Directory -Force $output
& $Compiler '/Qp' "/DTargetArchitecture=$Architecture" "/DAppExecutable=$executable" "/DAppVersion=$version" "/O$output" "$PSScriptRoot\installer.iss"
if ($LASTEXITCODE -ne 0) { throw "Installer compilation failed (exit $LASTEXITCODE)." }
$installer = Get-Item (Join-Path $output "GlowTray-$version-$Architecture-Setup.exe")
Write-Output ("Packaged {0}: {1:N0} bytes" -f $installer.FullName, $installer.Length)
