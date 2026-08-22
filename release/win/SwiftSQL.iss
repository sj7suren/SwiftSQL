; ============================================================================
;  SwiftSQL — Inno Setup script (Windows x64 installer)
;  Copyright (c) 2026 SwiftSQL Contributors · Apache License 2.0
;
;  Build with Inno Setup 6:  ISCC.exe SwiftSQL.iss   (or run build_installer.bat)
;
;  Highlights
;   • Bilingual installer (English default, 简体中文) — see [Languages].
;   • The language chosen at install time is written into SwiftSQL's own
;     settings.ini, so the app LAUNCHES in that same language (see [INI]).
;   • Per-user install (no admin), x64-only, self-contained single exe.
;   • Full uninstall: Control Panel entry + Start-menu shortcut (auto), plus an
;     optional prompt to also remove user data on uninstall (see [Code]).
;
;  C++ RUNTIME: none required. SwiftSQL.exe is built with the STATIC CRT (/MT),
;  so vcruntime140/msvcp140 are baked into the exe and nothing needs to be
;  installed on the target machine. (Oracle is NOT bundled; if a user needs
;  Oracle they drop a 64-bit oci.dll next to the exe or set the OCI path in-app.)
; ============================================================================

#define AppName        "SwiftSQL"
#define AppVersion     "1.1.20"
#define AppPublisher   "SwiftSQL Contributors"
#define AppExe         "SwiftSQL.exe"
#define AppUrl         "https://github.com/sj7suren/SwiftSQL"

[Setup]
; A stable, unique GUID identifies this product across upgrades — keep it fixed.
AppId={{B3F2A7D4-9C1E-4A6B-8E5F-2D7C4A1B9E30}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppUrl}
VersionInfoVersion=1.1.20.0
VersionInfoCompany={#AppPublisher}
VersionInfoDescription={#AppName} Setup
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
UninstallDisplayName={#AppName} {#AppVersion}
UninstallDisplayIcon={app}\SwiftSQL.ico
; Icon for the generated Setup.exe (the Inno-packaged installer) — SwiftSQL logo.
SetupIconFile=SwiftSQL.ico
OutputDir=Output
OutputBaseFilename=SwiftSQL-Setup-{#AppVersion}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern

; ---- Prerequisite / environment gates -------------------------------------
; Per-user install: no elevation, and {userappdata} resolves to the real user
; (so the language write below lands in the correct profile).
PrivilegesRequired=lowest
; x64-only product. Blocks 32-bit Windows with a clear message; allows x64 and
; ARM64 (x64 emulation). {autopf} therefore maps to 64-bit Program Files.
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Minimum OS. 6.1 = Windows 7 SP1 (permissive; the static build runs there).
; Raise to 10.0 if you want to require Windows 10+.
MinVersion=6.1sp1

; ---- Language selection ----------------------------------------------------
; English is listed first and detection is disabled, so the picker defaults to
; English ("首选英文") while still letting the user choose 简体中文.
ShowLanguageDialog=yes
LanguageDetectionMethod=none

[Languages]
; The English licence page shows the authoritative Apache-2.0 text straight from
; the repo root — one source of truth, no copy to drift. Inno resolves relative
; paths against this .iss file's directory, not the ISCC working directory.
Name: "en"; MessagesFile: "compiler:Default.isl";                     LicenseFile: "..\..\LICENSE"
; NOTE: ChineseSimplified.isl must exist in Inno's Languages folder. It ships
; with the Inno Setup 6 translations set; if missing, download it from
; https://github.com/jrsoftware/issrc/tree/main/Files/Languages
Name: "zh"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"; LicenseFile: "eula_zh.txt"

[CustomMessages]
en.InstallingVCRedist=Installing the Microsoft Visual C++ runtime...
zh.InstallingVCRedist=正在安装 Microsoft Visual C++ 运行库…
; Shown by the UNINSTALLER (in the language chosen at install time).
en.RemoveDataTitle=Remove SwiftSQL data
en.RemoveDataMsg=Do you also want to remove your SwiftSQL settings and saved data (connections, saved passwords, scripts, automation jobs)?%n%nChoose No to keep them for a future reinstall.
zh.RemoveDataTitle=删除 SwiftSQL 数据
zh.RemoveDataMsg=是否同时删除您的 SwiftSQL 设置与保存的数据（连接、已保存的密码、脚本、自动化作业）？%n%n选择“否”可保留，以便日后重新安装时继续使用。

[Tasks]
; Desktop shortcut is created BY DEFAULT (checkbox pre-ticked); user may untick.
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "dist\SwiftSQL.exe"; DestDir: "{app}"; Flags: ignoreversion
; Apache-2.0 section 4(a) requires every recipient of the Work to be GIVEN a copy
; of the licence. The wizard's licence page only DISPLAYS it, so install the file
; next to the exe as well.
Source: "..\..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
; App logo — installed so shortcuts and the uninstall entry show the SwiftSQL icon
; even though SwiftSQL.exe has no embedded icon yet.
Source: "SwiftSQL.ico"; DestDir: "{app}"; Flags: ignoreversion
; Optional external language-pack override directory (packs are also compiled
; into the exe, so this is only for drop-in overrides).
Source: "dist\lang\*"; DestDir: "{app}\lang"; Flags: ignoreversion recursesubdirs createallsubdirs
; VC++ 2015-2022 x64 runtime — bundled and silently installed ONLY if missing
; (see [Run] + VCRedistNeeded). Not strictly required by the static /MT exe, but
; shipped as a safeguard. skipifsourcedoesntexist keeps the script compilable even
; before the binary is downloaded into redist\.
Source: "redist\VC_redist.x64.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall skipifsourcedoesntexist

[INI]
; ---- Language propagation --------------------------------------------------
; Write ONLY the language key into SwiftSQL's settings.ini (all other settings
; are preserved). The app reads [general] language on startup, so it launches
; in whichever language was chosen in the installer.
Filename: "{userappdata}\{#AppName}\settings.ini"; Section: "general"; Key: "language"; String: "{code:LangCode}"

[Icons]
Name: "{group}\{#AppName}";                       Filename: "{app}\{#AppExe}"; IconFilename: "{app}\SwiftSQL.ico"
Name: "{group}\{cm:UninstallProgram,{#AppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}";                 Filename: "{app}\{#AppExe}"; Tasks: desktopicon; IconFilename: "{app}\SwiftSQL.ico"

[Run]
; Silently install the VC++ runtime ONLY when it is missing (VCRedistNeeded).
; On the vast majority of modern Windows it is already present → this never runs.
; When it does run on a clean machine it may raise one UAC prompt (the redist
; self-elevates, since this is a non-admin per-user install).
Filename: "{tmp}\VC_redist.x64.exe"; Parameters: "/install /quiet /norestart"; StatusMsg: "{cm:InstallingVCRedist}"; Check: VCRedistNeeded; Flags: skipifdoesntexist waituntilterminated
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; Remove the external lang override dir (created under {app}) if present, so no
; empty folder is left behind. Installed files are removed automatically.
Type: filesandordirs; Name: "{app}\lang"

[Code]
{ Map the chosen installer language (the [Languages] Name) to SwiftSQL's own
  language code. Both happen to be 'en'/'zh', but the mapping is explicit so the
  two are decoupled. }
function LangCode(Param: String): String;
begin
  if ActiveLanguage = 'zh' then
    Result := 'zh'
  else
    Result := 'en';
end;

{ True when the Microsoft Visual C++ 2015-2022 x64 runtime is NOT present, so the
  bundled redistributable should be installed. Checks the runtime's registry key
  first (64-bit view — we install in 64-bit mode), then falls back to probing for
  vcruntime140.dll in the system directory. }
function VCRedistNeeded: Boolean;
var
  Installed: Cardinal;
begin
  if RegQueryDWordValue(HKLM,
       'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64', 'Installed', Installed)
     and (Installed = 1) then
    Result := False
  else
    Result := not FileExists(ExpandConstant('{sys}\vcruntime140.dll'));
end;

{ On uninstall, offer to also delete the per-user data directory
  (%APPDATA%\SwiftSQL) — settings.ini, saved connections/passwords, scripts,
  favorites, automation jobs, crash log. Default is No (keep), because that data
  is valuable and users often reinstall. }
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  DataDir: String;
begin
  if CurUninstallStep = usUninstall then
  begin
    DataDir := ExpandConstant('{userappdata}\{#AppName}');
    if DirExists(DataDir) then
    begin
      if MsgBox(ExpandConstant('{cm:RemoveDataMsg}'),
                mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
        DelTree(DataDir, True, True, True);
    end;
  end;
end;
