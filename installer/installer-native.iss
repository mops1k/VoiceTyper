; Inno Setup script for the NATIVE VoiceTyper build (C++20 + Qt 6).
;
; Version and paths come from the command line (/DAppVersion=..., /DSourceDir=...,
; /DOutputDir=...), so one script serves every release - the same convention as the
; .NET installer script next to it.
;
; Three behaviours Alexander asked for explicitly (2026-10-06), and how each is done:
;   1. "the installer must fully uninstall the installed C# version and then install the
;      new one, and every installation must work that way":
;      this script keeps the SAME AppId as the .NET installer, which makes Inno Setup
;      run the previous version's uninstaller before installing - the standard upgrade
;      path, so it also holds for the next update of the native build itself.
;   2. "the application must close when the installer runs":
;      AppMutex below names the mutex the application creates
;      (kInstallerMutexName in src/app/tray_controller.cpp) and CloseApplications makes
;      Setup close it through the Restart Manager instead of failing on a locked exe.
;   3. "after the installer finishes the application must start again":
;      [Run] starts it, except during an automatic update (/AutoUpdate), where the
;      observer script the application wrote starts it after the installer exits - that
;      is what keeps a single instance from racing itself.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\build\windows-mingw-release\deploy"
#endif
#ifndef OutputDir
  #define OutputDir "..\build\windows-mingw-release"
#endif

[Setup]
; The same product identity as the .NET build: this is what makes Setup uninstall the
; previous (C#) installation first, and what keeps the uninstall entry single.
AppId={{9F22F58D-8CFB-4E7C-9D85-0B6B12D9A5E0}
AppName=VoiceTyper
AppVersion={#AppVersion}
AppVerName=VoiceTyper {#AppVersion}
AppPublisher=VoiceTyper
AppPublisherURL=https://github.com/mops1k/VoiceTyper
VersionInfoVersion={#AppVersion}
DefaultDirName={localappdata}\Programs\VoiceTyper
DefaultGroupName=VoiceTyper
DisableProgramGroupPage=yes
OutputDir={#OutputDir}
; The "win64" marker is what the application's updater checks before running a
; downloaded installer (kNativeInstallerMarker): it keeps the old .NET asset, which has
; no marker, from being installed over this build.
OutputBaseFilename=VoiceTyper-{#AppVersion}-win64-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayIcon={app}\VoiceTyper.exe
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
AppMutex=Global\VoiceTyper_SingleInstance
CloseApplications=yes
RestartApplications=no
; The application keeps its own settings and its downloaded models under
; %LOCALAPPDATA%\VoiceTyper, outside {app}, so an upgrade never touches them - the model
; files alone are hundreds of megabytes.

[Languages]
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"

[Tasks]
Name: "desktopicon"; Description: "Создать значок на рабочем столе"; GroupDescription: "Дополнительно:"

[Files]
; The application itself is installed under the product name the .NET build used, so the
; path in the update observer script and the shortcut never change across the switch.
Source: "{#SourceDir}\voicetyper-qt-shell.exe"; DestDir: "{app}"; DestName: "VoiceTyper.exe"; Flags: ignoreversion
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion; Excludes: "voicetyper-qt-shell.exe"

[Icons]
Name: "{group}\VoiceTyper"; Filename: "{app}\VoiceTyper.exe"
Name: "{autodesktop}\VoiceTyper"; Filename: "{app}\VoiceTyper.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\VoiceTyper.exe"; Description: "Запустить VoiceTyper"; Flags: nowait postinstall; Check: not IsAutoUpdate

[Code]
// An automatic update starts the installer with /AutoUpdate (see the observer script the
// application writes): the restart is left to that script, so the application is started
// exactly once, after the installer has finished.
function IsAutoUpdate: Boolean;
begin
  // Inno Setup has no built-in CmdLineParamExists, so the flag is looked for in the
  // whole command tail.
  Result := Pos('/AutoUpdate', GetCmdTail) > 0;
end;
