#ifndef AppVersion
  #error AppVersion must match the CMake project version
#endif
#ifndef PlayerExe
  #error PlayerExe must name the built player executable
#endif
#ifndef OutputPath
  #define OutputPath "..\build\installer"
#endif

[Setup]
AppId={{0A26A153-1274-493F-95DF-B1D75034851E}
AppName=Aino Player
AppVersion={#AppVersion}
AppPublisher=sudoevolve
AppPublisherURL=https://github.com/sudoevolve/aino-player
AppSupportURL=https://github.com/sudoevolve/aino-player/issues
AppUpdatesURL=https://github.com/sudoevolve/aino-player/releases
DefaultDirName={localappdata}\Programs\Aino Player
DefaultGroupName=Aino Player
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir={#OutputPath}
OutputBaseFilename=aino-player-v{#AppVersion}-setup-windows-x64
SetupIconFile=..\assets\aino.ico
UninstallDisplayIcon={app}\aino_player.exe
LicenseFile=..\LICENSE
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
DisableProgramGroupPage=yes
UsePreviousAppDir=yes
CloseApplications=yes
CloseApplicationsFilter=aino_player.exe
RestartApplications=no
UninstallDisplayName=Aino Player

[Tasks]
Name: desktopicon; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts:"; Flags: unchecked

[Files]
Source: "{#PlayerExe}"; DestDir: "{app}"; DestName: "aino_player.exe"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}\licenses"; Flags: ignoreversion
Source: "..\LICENSE-aino"; DestDir: "{app}\licenses"; Flags: ignoreversion
Source: "..\vendor\LICENSE.FFmpeg.txt"; DestDir: "{app}\licenses"; Flags: ignoreversion
Source: "..\vendor\README.md"; DestDir: "{app}\licenses"; DestName: "Third-party.md"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\Aino Player"; Filename: "{app}\aino_player.exe"; AppUserModelID: "sudoevolve.AinoPlayer"
Name: "{autodesktop}\Aino Player"; Filename: "{app}\aino_player.exe"; Tasks: desktopicon; AppUserModelID: "sudoevolve.AinoPlayer"

[Run]
Filename: "{app}\aino_player.exe"; Description: "Launch Aino Player"; Flags: nowait postinstall skipifsilent; Check: not FromUpdater
Filename: "{app}\aino_player.exe"; Flags: nowait; Check: FromUpdater

[Code]
function FromUpdater: Boolean;
begin
  Result := ExpandConstant('{param:AINORESTART|0}') = '1';
end;

// Config, cookies, favorites and downloaded music live outside {app}; uninstall preserves them.
