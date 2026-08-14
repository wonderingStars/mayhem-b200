; mayhem-b200 Windows installer (Inno Setup 6).
;
; Builds a self-contained setup.exe: the app + web portal + the full UHD runtime
; (uhd.dll, libusb, B200 FPGA/firmware images) so a clean machine needs no
; separate UHD install. Creates Desktop + Start Menu shortcuts and an uninstaller.
;
; Compile:  "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" installer\mayhem-b200.iss
;
; SPDX-License-Identifier: GPL-2.0-or-later

#define AppName "Mayhem B200"
#define AppVersion "0.19.0"
#define AppPublisher "mayhem-b200 contributors"
#define AppURL "https://github.com/wonderingStars/mayhem-b200"
#define AppExe "mayhem-b200.exe"

[Setup]
AppId={{7A3F2C10-9E4B-4D2A-8C1F-3B5E6A7D9C20}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=auto
LicenseFile=staging\core\LICENSE.txt
OutputDir=output
OutputBaseFilename=mayhem-b200-{#AppVersion}-setup
SetupIconFile=assets\mayhem-b200.ico
UninstallDisplayIcon={app}\mayhem-b200.ico
UninstallDisplayName={#AppName} {#AppVersion}
WizardStyle=modern
Compression=lzma2/ultra
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Types]
Name: "full"; Description: "Full — B200/USRP and other SDRs (SoapySDR)"
Name: "compact"; Description: "B200 / USRP only"
Name: "custom"; Description: "Custom"; Flags: iscustom

[Components]
Name: "core"; Description: "Mayhem B200 — B200 / USRP support (required)"; Types: full compact custom; Flags: fixed
Name: "soapy"; Description: "Other SDRs via SoapySDR — RTL-SDR, HackRF, Airspy, Pluto, Lime, BladeRF (+ Zadig)"; Types: full

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
; Core payload: app, portal, UHD runtime, launcher, images.
Source: "staging\core\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion; Components: core
Source: "assets\mayhem-b200.ico"; DestDir: "{app}"; Flags: ignoreversion; Components: core
; Optional: full SoapySDR stack (sdrlink server + worker + SoapySDR runtime, all
; device modules, native libs, and Zadig) — into an {app}\soapy subfolder.
Source: "staging\soapy\*"; DestDir: "{app}\soapy"; Flags: recursesubdirs ignoreversion; Components: soapy

[Icons]
; Full experience (radio + portal + browser), via the windowless VBScript launcher.
Name: "{group}\{#AppName}"; Filename: "{sys}\wscript.exe"; Parameters: """{app}\launch.vbs"""; WorkingDir: "{app}"; IconFilename: "{app}\mayhem-b200.ico"; Comment: "Start the radio and open the web portal"
Name: "{autodesktop}\{#AppName}"; Filename: "{sys}\wscript.exe"; Parameters: """{app}\launch.vbs"""; WorkingDir: "{app}"; IconFilename: "{app}\mayhem-b200.ico"; Comment: "Start the radio and open the web portal"; Tasks: desktopicon
; The native app window only (no portal) — useful for --driver=sdrlink or a bare session.
Name: "{group}\{#AppName} (app window only)"; Filename: "{app}\{#AppExe}"; Parameters: "--driver=uhd"; WorkingDir: "{app}"; IconFilename: "{app}\mayhem-b200.ico"; Comment: "Run the native app window without the web portal"
; SoapySDR component shortcuts.
Name: "{group}\{#AppName} (other SDRs)"; Filename: "{sys}\wscript.exe"; Parameters: """{app}\soapy\launch-other-sdrs.vbs"""; WorkingDir: "{app}\soapy"; IconFilename: "{app}\mayhem-b200.ico"; Comment: "Use RTL-SDR / HackRF / Airspy / Pluto / Lime / BladeRF via SoapySDR"; Components: soapy
Name: "{group}\Set up USB SDR driver (Zadig)"; Filename: "{app}\soapy\zadig-2.5.exe"; WorkingDir: "{app}\soapy"; Comment: "Assign the WinUSB driver to a USB SDR (one-time, per radio)"; Components: soapy
Name: "{group}\USB SDR driver guide"; Filename: "{app}\soapy\ZADIG-README.txt"; Components: soapy
Name: "{group}\Uninstall {#AppName}"; Filename: "{uninstallexe}"

[Run]
Filename: "{sys}\wscript.exe"; Parameters: """{app}\launch.vbs"""; WorkingDir: "{app}"; Description: "Launch {#AppName} now"; Flags: postinstall nowait skipifsilent

[UninstallDelete]
; The app never writes into {app} at runtime (state lives in %LOCALAPPDATA%), so
; nothing extra to purge here beyond what the installer placed.
