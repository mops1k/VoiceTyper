; Скрипт Inno Setup для сборки установщика VoiceTyper.
; Версия и пути подаются через define (/DAppVersion=..., /DSourceDir=..., /DOutputDir=...),
; чтобы один скрипт переиспользовать для разных версий.
;
; Пути по умолчанию относительны папки installer\ ; CI переопределяет их через /D.

#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\artifacts\publish"
#endif
#ifndef OutputDir
  #define OutputDir "..\artifacts\installer"
#endif

[Setup]
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
OutputBaseFilename=VoiceTyper-{#AppVersion}-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayIcon={app}\VoiceTyper.exe
; Установка и запуск без прав администратора (per-user).
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
; Запрет установки/удаления, пока VoiceTyper запущен (имя mutex'а из App.xaml.cs).
AppMutex=Global\VoiceTyper_SingleInstance
; Закрываем приложение (Windows Restart Manager) при замене выполняемых файлов
; и не перезапускаем его сами — запуск делает секция [Run] ниже.
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"

[Tasks]
Name: "desktopicon"; Description: "Создать значок на рабочем столе"; GroupDescription: "Дополнительно:"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion

[Icons]
Name: "{group}\VoiceTyper"; Filename: "{app}\VoiceTyper.exe"
Name: "{autodesktop}\VoiceTyper"; Filename: "{app}\VoiceTyper.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\VoiceTyper.exe"; Description: "Запустить VoiceTyper"; Flags: nowait postinstall; Check: not IsAutoUpdate

[Code]
// Авто-обновление запускает установщик с /AutoUpdate (см. UpdateLauncher в приложении):
// перезапуск приложения после установки делает сам наблюдатель, чтобы не было двойного запуска.
function IsAutoUpdate: Boolean;
begin
  // Встроенной CmdLineParamExists в Inno Setup нет, поэтому ищем флаг в полной командной строке.
  Result := Pos('/AutoUpdate', GetCmdTail) > 0;
end;
