#define ProductName "FlexWeb"
#ifndef ProductVersion
#define ProductVersion "1.0.0"
#endif
#ifndef OutputDir
#define OutputDir "."
#endif
[Setup]
AppId={{8F0AB9F0-06EB-4C20-91C7-9D5216A76142}
AppName={#ProductName}
AppVersion={#ProductVersion}
AppPublisher=FlexWeb
DefaultDirName={localappdata}\Programs\FlexWeb
DefaultGroupName=FlexWeb
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutputDir}
OutputBaseFilename=FlexWeb-Setup-{#ProductVersion}-windows-x64
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\wfweb-launcher.exe
[Files]
Source: "..\FlexWeb\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
[Icons]
Name: "{group}\FlexWeb"; Filename: "{app}\wfweb-launcher.exe"; WorkingDir: "{app}"
Name: "{group}\Uninstall FlexWeb"; Filename: "{uninstallexe}"
[Run]
Filename: "{app}\wfweb-launcher.exe"; Description: "Launch FlexWeb"; Flags: nowait postinstall skipifsilent
