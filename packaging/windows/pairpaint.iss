; Inno Setup script for the PairPaint Windows installer.
; Built by the release workflow:  iscc /DAppVersion=0.1.0 /DSourceDir=<deployed app folder> pairpaint.iss

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\..\dist\PairPaint"
#endif

[Setup]
; Keep AppId the same in every release so upgrades replace the previous version.
AppId={{1FABB57F-F7C5-49D9-A9C6-370E513B5834}
AppName=PairPaint
AppVersion={#AppVersion}
AppVerName=PairPaint {#AppVersion}
AppPublisher=Peter Gniewek
AppPublisherURL=https://github.com/pik10/pairpaint
AppSupportURL=https://github.com/pik10/pairpaint/issues
DefaultDirName={autopf}\PairPaint
DefaultGroupName=PairPaint
DisableProgramGroupPage=yes
LicenseFile={#SourceDir}\LICENSE.txt
OutputBaseFilename=PairPaint-{#AppVersion}-windows-x64-setup
SetupIconFile=pairpaint.ico
UninstallDisplayIcon={app}\pairpaint.exe
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Installs for the current user without admin rights unless the user chooses all users.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ChangesAssociations=yes
Compression=lzma2
SolidCompression=yes
WizardStyle=modern

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "associate"; Description: "Open .pairpaint projects with PairPaint"; GroupDescription: "File associations:"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion

[Icons]
Name: "{autoprograms}\PairPaint"; Filename: "{app}\pairpaint.exe"
Name: "{autodesktop}\PairPaint"; Filename: "{app}\pairpaint.exe"; Tasks: desktopicon

[Registry]
Root: HKA; Subkey: "Software\Classes\.pairpaint"; ValueType: string; ValueName: ""; ValueData: "PairPaint.Project"; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PairPaint.Project"; ValueType: string; ValueName: ""; ValueData: "PairPaint Project"; Flags: uninsdeletekey; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PairPaint.Project\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\pairpaint.exe,0"; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PairPaint.Project\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\pairpaint.exe"" ""%1"""; Tasks: associate

[Run]
Filename: "{app}\pairpaint.exe"; Description: "{cm:LaunchProgram,PairPaint}"; Flags: nowait postinstall skipifsilent
