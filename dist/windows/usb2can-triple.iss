; SPDX-License-Identifier: MIT
; Copyright (c) 2026 CANLAB s.r.o.
; Inno Setup script of the Windows installer; dist\windows\build.ps1 compiles it with
;   ISCC /DAppVersion=<version> /DStageDir=<staged files> /DOutDir=<output> /DIconFile=<.ico>

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif

[Setup]
AppId={{6F1C0E53-4B1D-4C55-9A0B-2B7C2E4C7A11}
AppName=USB2CAN Triple
AppVersion={#AppVersion}
AppVerName=USB2CAN Triple {#AppVersion}
AppPublisher=CANLAB s.r.o.
AppPublisherURL=https://www.canlab.cz
AppSupportURL=https://github.com/canlab-cz/triple
DefaultDirName={autopf}\USB2CAN Triple
DefaultGroupName=USB2CAN Triple
DisableProgramGroupPage=yes
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutDir}
OutputBaseFilename=usb2can-triple-{#AppVersion}-windows-x64-setup
SetupIconFile={#IconFile}
UninstallDisplayIcon={app}\SavvyCAN\SavvyCAN.exe
LicenseFile={#StageDir}\LICENSES-WINDOWS.txt
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ChangesEnvironment=yes

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"
Name: "cs"; MessagesFile: "compiler:Languages\Czech.isl"

[CustomMessages]
en.ToolsPath=Add the command line tools (canre_cli) to the PATH
cs.ToolsPath=Přidat nástroje příkazové řádky (canre_cli) do proměnné PATH
en.Docs=Documentation
cs.Docs=Dokumentace

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"
Name: "toolspath"; Description: "{cm:ToolsPath}"; Flags: unchecked

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\SavvyCAN (USB2CAN Triple)"; Filename: "{app}\SavvyCAN\SavvyCAN.exe"; WorkingDir: "{app}\SavvyCAN"
Name: "{group}\{cm:Docs}"; Filename: "{app}\doc"
Name: "{group}\{cm:UninstallProgram,USB2CAN Triple}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\SavvyCAN (USB2CAN Triple)"; Filename: "{app}\SavvyCAN\SavvyCAN.exe"; WorkingDir: "{app}\SavvyCAN"; Tasks: desktopicon

[Registry]
Root: HKA; Subkey: "{code:EnvKey}"; ValueType: expandsz; ValueName: "Path"; ValueData: "{olddata};{app}\tools"; Tasks: toolspath; Check: NeedsAddPath(ExpandConstant('{app}\tools'))

[Run]
Filename: "{app}\SavvyCAN\SavvyCAN.exe"; Description: "{cm:LaunchProgram,SavvyCAN}"; Flags: nowait postinstall skipifsilent

[Code]
function EnvKey(Param: String): String;
begin
  if IsAdminInstallMode then
    Result := 'SYSTEM\CurrentControlSet\Control\Session Manager\Environment'
  else
    Result := 'Environment';
end;

function NeedsAddPath(Dir: String): Boolean;
var
  Path: String;
  Root: Integer;
begin
  if IsAdminInstallMode then Root := HKEY_LOCAL_MACHINE else Root := HKEY_CURRENT_USER;
  if not RegQueryStringValue(Root, EnvKey(''), 'Path', Path) then
    Path := '';
  Result := Pos(';' + Uppercase(Dir) + ';', ';' + Uppercase(Path) + ';') = 0;
end;

procedure RemovePath(Dir: String);
var
  Path: String;
  Root, P: Integer;
begin
  if IsAdminInstallMode then Root := HKEY_LOCAL_MACHINE else Root := HKEY_CURRENT_USER;
  if not RegQueryStringValue(Root, EnvKey(''), 'Path', Path) then
    Exit;
  P := Pos(';' + Uppercase(Dir), Uppercase(Path));
  if P > 0 then
  begin
    Delete(Path, P, Length(Dir) + 1);
    RegWriteExpandStringValue(Root, EnvKey(''), 'Path', Path);
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
    RemovePath(ExpandConstant('{app}\tools'));
end;
