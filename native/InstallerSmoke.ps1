param(
    [Parameter(Mandatory)][string]$Installer,
    [ValidateSet('x64', 'arm64')][string]$Architecture = 'x64'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$Installer = (Resolve-Path $Installer).Path
$source = (Resolve-Path (Join-Path $root "artifacts\GlowTray\$Architecture\GlowTray.exe")).Path
$startupPath = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$uninstallPath = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\{4EB714F1-54D1-45AB-BF37-38D24B403EE4}_is1'
$data = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'GlowTray'
$shortcut = Join-Path ([Environment]::GetFolderPath('Programs')) 'GlowTray.lnk'
$startup = Get-ItemProperty $startupPath -ErrorAction SilentlyContinue
if ((Test-Path $data) -or (Test-Path $uninstallPath) -or (Test-Path $shortcut) -or
    ($startup -and ($startup.PSObject.Properties['GlowTray'] -or $startup.PSObject.Properties['RingLightNative'])) -or
    (Get-Process GlowTray -ErrorAction SilentlyContinue)) {
    throw 'Installer smoke checks require a clean Windows account without GlowTray settings, startup entries, or installation.'
}
$work = Join-Path $root "artifacts\installer-smoke\$Architecture"
$destination = Join-Path $work 'Installed GlowTray'
$payload = Join-Path $destination 'GlowTray.exe'
$uninstaller = Join-Path $destination 'unins000.exe'
$null = New-Item -ItemType Directory -Force $work
$arguments = @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-')

function Invoke-Setup([string]$Program, [string]$Log, [switch]$Blocked) {
    $options = $arguments + @("/LOG=`"$work\$Log.log`"")
    if ($Program -eq $Installer) { $options += "/DIR=`"$destination`"" }
    $process = Start-Process $Program -ArgumentList $options -NoNewWindow -Wait -PassThru
    if ($null -eq $process.ExitCode) { throw "$Log did not return an installer exit code." }
    if ($Blocked) {
        if ($process.ExitCode -eq 0) { throw "$Log unexpectedly succeeded while GlowTray's mutex existed." }
    } elseif ($process.ExitCode -ne 0) {
        throw "$Log failed (exit $($process.ExitCode)); see $work\$Log.log."
    }
}
function Assert-Payload {
    if ((Get-FileHash $payload).Hash -ne (Get-FileHash $source).Hash) { throw 'Installed executable differs from the portable build.' }
    if (-not (Test-Path $uninstallPath) -or -not (Test-Path $shortcut)) { throw 'Per-user uninstall registration or Start menu shortcut is missing.' }
}

$created = $false
$mutex = [Threading.Mutex]::new($false, 'Local\GlowTray', [ref]$created)
try {
    if (-not $created) { throw 'GlowTray is already running.' }
    Invoke-Setup $Installer 'blocked-install' -Blocked
    if (Test-Path $payload) { throw 'Blocked installation wrote the application executable.' }
}
finally { $mutex.Dispose() }

Invoke-Setup $Installer 'install'
Assert-Payload
if ((Get-ItemProperty $startupPath -Name GlowTray -ErrorAction SilentlyContinue)) { throw 'Installation enabled startup without consent.' }

$null = New-Item -ItemType Directory $data
$settings = Join-Path $data 'settings.json'
$fixture = '{"devices":[],"selected":"","installerSmoke":true}'
[IO.File]::WriteAllText($settings, $fixture)
$null = New-Item -Path $startupPath -Force
New-ItemProperty $startupPath -Name GlowTray -Value '"C:\previous\GlowTray.exe" --tray' -PropertyType String -Force | Out-Null
Invoke-Setup $Installer 'upgrade'
Assert-Payload
$expected = "`"$payload`" --tray"
if ((Get-ItemPropertyValue $startupPath -Name GlowTray) -ne $expected) { throw 'Upgrade did not preserve and retarget enabled startup.' }
if ([IO.File]::ReadAllText($settings) -ne $fixture) { throw 'Upgrade changed saved settings.' }

$mutex = [Threading.Mutex]::new($false, 'Local\GlowTray', [ref]$created)
try {
    if (-not $created) { throw 'GlowTray is already running.' }
    Invoke-Setup $uninstaller 'blocked-uninstall' -Blocked
    Assert-Payload
}
finally { $mutex.Dispose() }

Invoke-Setup $uninstaller 'uninstall'
if ((Test-Path $payload) -or (Test-Path $uninstallPath) -or (Test-Path $shortcut)) { throw 'Uninstall left application files, registration, or shortcuts.' }
if ((Get-ItemProperty $startupPath -Name GlowTray -ErrorAction SilentlyContinue)) { throw 'Uninstall left a startup entry pointing at the removed executable.' }
if ([IO.File]::ReadAllText($settings) -ne $fixture) { throw 'Uninstall removed or changed saved settings.' }

Invoke-Setup $Installer 'reinstall'
Assert-Payload
if ((Get-ItemProperty $startupPath -Name GlowTray -ErrorAction SilentlyContinue)) { throw 'Reinstallation enabled startup without consent.' }
$portableStartup = '"C:\portable\GlowTray.exe" --tray'
New-ItemProperty $startupPath -Name GlowTray -Value $portableStartup -PropertyType String -Force | Out-Null
Invoke-Setup $uninstaller 'uninstall-with-portable-startup'
if ((Test-Path $payload) -or (Test-Path $uninstallPath) -or (Test-Path $shortcut)) { throw 'Second uninstall left application files, registration, or shortcuts.' }
if ((Get-ItemPropertyValue $startupPath -Name GlowTray) -ne $portableStartup) { throw 'Uninstall changed the portable app startup entry.' }
if ([IO.File]::ReadAllText($settings) -ne $fixture) { throw 'Second uninstall changed saved settings.' }
Remove-ItemProperty $startupPath -Name GlowTray
Remove-Item $settings
[IO.Directory]::Delete($data)
Write-Output "Installer smoke checks passed for $Architecture."
