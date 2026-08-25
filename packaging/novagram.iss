#ifndef BuildDir
#define BuildDir "D:\Documents\Coding\NovaGram\desktop\novagram-desktop\out"
#endif

#ifndef OutputDir
#define OutputDir "D:\Documents\Coding\NovaGram\dist"
#endif

#define MyAppName "NovaGram"
#define MyAppPublisher "Brent"
; The Telegram Desktop base, which is also what NovaGram releases under on this
; platform. The update check compares this number, so it has to match
; NovaGram::AppVersion(), that is Telegram/build/version.
#define MyAppVersion "7.1.2"
#define MyAppExeName "NovaGram.exe"

[Setup]
AppId={{B2F38D7E-01A7-4B7E-8B1D-52D87A75B5B2}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={localappdata}\Programs\NovaGram
DefaultGroupName=NovaGram
OutputDir={#OutputDir}
OutputBaseFilename=NovaGramSetup-{#MyAppVersion}-x64
Compression=lzma2/ultra64
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=lowest
UninstallDisplayIcon={app}\{#MyAppExeName}
SetupLogging=yes
; The in-app updater starts this while NovaGram is still shutting down, so the
; installer has to be allowed to close what is left of it before replacing the
; executable. Nothing under the data directory is touched either way.
CloseApplications=yes
RestartApplications=no
#ifdef UseSigning
SignedUninstaller=yes
SignTool=brent-sign
#endif

[Files]
Source: "{#BuildDir}\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\NovaGram"; Filename: "{app}\{#MyAppExeName}"; Comment: "NovaGram"
Name: "{autodesktop}\NovaGram"; Filename: "{app}\{#MyAppExeName}"; Comment: "NovaGram"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create desktop shortcut"; GroupDescription: "Shortcuts:"; Flags: unchecked

[Run]
; No skipifsilent on purpose. The in-app updater runs this installer with
; /VERYSILENT, and without this entry running in silent mode too the update
; would replace the program and leave the user staring at a closed window.
Filename: "{app}\{#MyAppExeName}"; Description: "Launch NovaGram"; Flags: nowait postinstall
