; NSIS installer for shcl (Windows). Driven by cicd/utility/package.bash:
;   makensis -DVERSION=... -DSRCEXE=... -DPAYLOAD=... -DOUTFILE=... shcl.nsi
; Layout matches the system-install spec: $PROGRAMFILES64\Shcl with the binary,
; code\ (drop-in files), scripts\ (ps1 wrapper), added to the machine PATH.

!ifndef VERSION
	!error "pass /DVERSION="
!endif
!ifndef SRCEXE
	!error "pass /DSRCEXE="
!endif
!ifndef PAYLOAD
	!error "pass /DPAYLOAD="
!endif
!ifndef OUTFILE
	!error "pass /DOUTFILE="
!endif

Unicode true
Name "SHCL ${VERSION}"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\Shcl"
RequestExecutionLevel admin
SetCompressor /SOLID lzma

!include "WinMessages.nsh"

!define REG_UNINST "Software\Microsoft\Windows\CurrentVersion\Uninstall\Shcl"

; The machine PATH is edited by PowerShell against the registry directly, never
; through NSIS variables: NSIS strings are capped at NSIS_MAX_STRLEN (1024), so
; a longer PATH came back truncated - or empty, which no length guard can tell
; from a genuinely empty value - and writing that back destroyed it. The script
; also compares whole segments case-insensitively (a substring test let any
; directory containing the name suppress the append) and keeps REG_EXPAND_SZ.
!macro WriteShclPathPs1
	FileOpen $0 "$PLUGINSDIR\shclpath.ps1" w
	FileWrite $0 "param([string]$$Dir, [switch]$$Remove)$\r$\n"
	FileWrite $0 "$$key = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SYSTEM\CurrentControlSet\Control\Session Manager\Environment', $$true)$\r$\n"
	FileWrite $0 "$$cur = [string]$$key.GetValue('Path', '', [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)$\r$\n"
	FileWrite $0 "$$parts = @($$cur -split ';' | Where-Object { $$_ -ne '' })$\r$\n"
	FileWrite $0 "if ($$Remove) {$\r$\n"
	FileWrite $0 "	$$new = @($$parts | Where-Object { $$_ -ne $$Dir }) -join ';'$\r$\n"
	FileWrite $0 "	if ($$new -ne $$cur) { $$key.SetValue('Path', $$new, [Microsoft.Win32.RegistryValueKind]::ExpandString) }$\r$\n"
	FileWrite $0 "} elseif ($$parts -notcontains $$Dir) {$\r$\n"
	FileWrite $0 "	$$key.SetValue('Path', (@($$parts + $$Dir) -join ';'), [Microsoft.Win32.RegistryValueKind]::ExpandString)$\r$\n"
	FileWrite $0 "}$\r$\n"
	FileWrite $0 "$$key.Close()$\r$\n"
	FileClose $0
!macroend

Section "Install"
	SetOutPath "$INSTDIR"
	File "/oname=shcl.exe" "${SRCEXE}"
	SetOutPath "$INSTDIR\code"
	File "${PAYLOAD}\code\*.*"
	SetOutPath "$INSTDIR\scripts"
	File "${PAYLOAD}\scripts\*.*"
	WriteUninstaller "$INSTDIR\uninstall.exe"

	WriteRegStr HKLM "${REG_UNINST}" "DisplayName" "SHCL"
	WriteRegStr HKLM "${REG_UNINST}" "DisplayVersion" "${VERSION}"
	WriteRegStr HKLM "${REG_UNINST}" "Publisher" "Jim Collier"
	WriteRegStr HKLM "${REG_UNINST}" "URLInfoAbout" "https://github.com/jim-collier/shcl"
	WriteRegStr HKLM "${REG_UNINST}" "InstallLocation" "$INSTDIR"
	WriteRegStr HKLM "${REG_UNINST}" "UninstallString" '"$INSTDIR\uninstall.exe"'
	WriteRegDWORD HKLM "${REG_UNINST}" "NoModify" 1
	WriteRegDWORD HKLM "${REG_UNINST}" "NoRepair" 1

	; Append to the machine PATH once (segment-wise, REG_EXPAND_SZ preserved).
	InitPluginsDir
	!insertmacro WriteShclPathPs1
	nsExec::ExecToStack 'powershell -NoProfile -ExecutionPolicy Bypass -File "$PLUGINSDIR\shclpath.ps1" -Dir "$INSTDIR"'
	Pop $1
	StrCmp $1 "0" +2
	DetailPrint "PATH update failed; add $INSTDIR to the machine PATH manually"
	SendMessage ${HWND_BROADCAST} ${WM_WININICHANGE} 0 "STR:Environment" /TIMEOUT=5000
SectionEnd

Section "Uninstall"
	; Best-effort segment-wise PATH removal, then the files.
	InitPluginsDir
	!insertmacro WriteShclPathPs1
	nsExec::ExecToStack 'powershell -NoProfile -ExecutionPolicy Bypass -File "$PLUGINSDIR\shclpath.ps1" -Dir "$INSTDIR" -Remove'
	Pop $1
	StrCmp $1 "0" +2
	DetailPrint "PATH cleanup failed; remove $INSTDIR from the machine PATH manually"
	SendMessage ${HWND_BROADCAST} ${WM_WININICHANGE} 0 "STR:Environment" /TIMEOUT=5000
	Delete "$INSTDIR\shcl.exe"
	Delete "$INSTDIR\code\*.*"
	Delete "$INSTDIR\scripts\*.*"
	RMDir "$INSTDIR\code"
	RMDir "$INSTDIR\scripts"
	Delete "$INSTDIR\uninstall.exe"
	RMDir "$INSTDIR"
	DeleteRegKey HKLM "${REG_UNINST}"
SectionEnd
