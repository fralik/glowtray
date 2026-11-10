$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class TrayProbe {
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct Data {
        public uint Size;
        public IntPtr Window;
        public uint Id, Flags, Message;
        public IntPtr Icon;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string Tip;
        public uint State, StateMask;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string Info;
        public uint Version;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)] public string Title;
        public uint InfoFlags;
        public Guid Guid;
        public IntPtr BalloonIcon;
    }
    [DllImport("shell32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool Shell_NotifyIcon(uint message, ref Data data);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr LoadIcon(IntPtr instance, IntPtr name);
}
'@
Write-Output "PowerShell architecture: $([Runtime.InteropServices.RuntimeInformation]::ProcessArchitecture)"
Write-Output "Session: $((Get-Process -Id $PID).SessionId)"
foreach ($hive in @('HKCU:', 'HKLM:')) {
    $policy = Get-ItemProperty "$hive\Software\Microsoft\Windows\CurrentVersion\Policies\Explorer" -ErrorAction SilentlyContinue
    Write-Output "$hive NoTrayItemsDisplay: $($policy.NoTrayItemsDisplay)"
}
Get-Process explorer,WWAHost,StartMenuExperienceHost,UserOOBEBroker,SystemPropertiesPerformance -ErrorAction SilentlyContinue |
    Select-Object Id,ProcessName,SessionId,MainWindowTitle | Format-Table | Out-String | Write-Output
foreach ($directory in @('C:\a', $env:GITHUB_WORKSPACE, $env:RUNNER_TEMP)) {
    if ($directory -and (Test-Path $directory)) {
        Get-Item $directory | Select-Object FullName,LinkType,Target | Format-List | Out-String | Write-Output
    }
}
$form = [Windows.Forms.Form]::new()
try {
    foreach ($useGuid in @($false, $true)) {
        $data = [TrayProbe+Data]::new()
        $data.Size = [Runtime.InteropServices.Marshal]::SizeOf($data)
        $data.Window = $form.Handle
        $data.Id = 1
        $data.Flags = 7
        if ($useGuid) { $data.Flags = $data.Flags -bor 32 }
        $data.Message = 32770
        $data.Icon = [TrayProbe]::LoadIcon([IntPtr]::Zero, [IntPtr]32512)
        $data.Tip = 'Tray API probe'
        $data.Guid = [Guid]::NewGuid()
        $added = [TrayProbe]::Shell_NotifyIcon(0, [ref]$data)
        $errorCode = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        Write-Output "Tray probe: GUID=$useGuid size=$($data.Size) added=$added error=$errorCode"
        if ($added) { $null = [TrayProbe]::Shell_NotifyIcon(2, [ref]$data) }
    }
}
finally { $form.Dispose() }
