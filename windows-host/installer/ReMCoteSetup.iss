; ReMCote Windows installer — built with Inno Setup 6 (preinstalled on GitHub runners).
; Produces: output\ReMCoteSetup.exe
;
; Expects before compilation:
;   ..\dist\ReMCoteHost.exe       (compiled host)
;   vc_redist.x64.exe             (downloaded in CI, next to this script)

[Setup]
AppId={{8E2A67C1-4B5E-4C8F-9D3A-RemCote00001}
AppName=ReMCote
AppVersion=0.1.0
AppPublisher=ReMCote
AppPublisherURL=https://remcote.replit.app
DefaultDirName={autopf}\ReMCote
DefaultGroupName=ReMCote
DisableProgramGroupPage=yes
UninstallDisplayIcon={app}\ReMCoteHost.exe
OutputBaseFilename=ReMCoteSetup
OutputDir=output
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
WizardStyle=modern

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional shortcuts:"

[Files]
Source: "..\dist\ReMCoteHost.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "vc_redist.x64.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall

[Icons]
Name: "{group}\ReMCote"; Filename: "{app}\ReMCoteHost.exe"
Name: "{autodesktop}\ReMCote"; Filename: "{app}\ReMCoteHost.exe"; Tasks: desktopicon

[Run]
Filename: "{tmp}\vc_redist.x64.exe"; Parameters: "/install /quiet /norestart"; StatusMsg: "Installing Microsoft runtime..."; Flags: waituntilterminated
Filename: "{app}\ReMCoteHost.exe"; Description: "Launch ReMCote"; Flags: nowait postinstall skipifsilent
