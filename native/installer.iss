#ifndef TargetArchitecture
  #error Run package.ps1 to supply the target architecture and application metadata.
#endif

[Setup]
AppId={{4EB714F1-54D1-45AB-BF37-38D24B403EE4}
AppName=GlowTray
AppVersion={#AppVersion}
AppPublisher=fralik
AppPublisherURL=https://github.com/fralik/glowtray
DefaultDirName={localappdata}\Programs\GlowTray
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
MinVersion=10.0
#if TargetArchitecture == "x64"
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
#elif TargetArchitecture == "arm64"
ArchitecturesAllowed=arm64
ArchitecturesInstallIn64BitMode=arm64
#else
  #error Unsupported target architecture.
#endif
AppMutex=Local\GlowTray
CloseApplications=no
RestartApplications=no
UninstallDisplayIcon={app}\GlowTray.exe
OutputBaseFilename=GlowTray-{#AppVersion}-{#TargetArchitecture}-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern

[Files]
Source: "{#AppExecutable}"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\GlowTray"; Filename: "{app}\GlowTray.exe"

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "GlowTray"; ValueData: """{app}\GlowTray.exe"" --tray"; Check: StartupEnabled

[Run]
Filename: "{app}\GlowTray.exe"; Description: "Launch GlowTray"; Flags: nowait postinstall skipifsilent

[Code]
const
  StartupKey = 'Software\Microsoft\Windows\CurrentVersion\Run';

function StartupEnabled: Boolean;
begin
  Result := RegValueExists(HKCU, StartupKey, 'GlowTray');
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Command: String;
begin
  if CurUninstallStep = usUninstall then
    if RegQueryStringValue(HKCU, StartupKey, 'GlowTray', Command) then
      if CompareText(Command, '"' + ExpandConstant('{app}\GlowTray.exe') + '" --tray') = 0 then
        if not RegDeleteValue(HKCU, StartupKey, 'GlowTray') then
          RaiseException('Could not remove the GlowTray startup entry.');
end;
