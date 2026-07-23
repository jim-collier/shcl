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
!include "StrFunc.nsh"
${Using:StrFunc} StrStr
${Using:StrFunc} UnStrRep

!define REG_UNINST "Software\Microsoft\Windows\CurrentVersion\Uninstall\Shcl"
!define REG_ENV "SYSTEM\CurrentControlSet\Control\Session Manager\Environment"

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

	; Append to the machine PATH once; keep the value REG_EXPAND_SZ.
	ReadRegStr $0 HKLM "${REG_ENV}" "Path"
	${StrStr} $1 "$0" "$INSTDIR"
	StrCmp $1 "" 0 pathDone
	WriteRegExpandStr HKLM "${REG_ENV}" "Path" "$0;$INSTDIR"
	SendMessage ${HWND_BROADCAST} ${WM_WININICHANGE} 0 "STR:Environment" /TIMEOUT=5000
pathDone:
SectionEnd

Section "Uninstall"
	; Best-effort PATH removal (both separator spellings), then the files.
	ReadRegStr $0 HKLM "${REG_ENV}" "Path"
	${UnStrRep} $1 "$0" ";$INSTDIR" ""
	${UnStrRep} $1 "$1" "$INSTDIR;" ""
	StrCmp $1 "$0" envDone
	WriteRegExpandStr HKLM "${REG_ENV}" "Path" "$1"
	SendMessage ${HWND_BROADCAST} ${WM_WININICHANGE} 0 "STR:Environment" /TIMEOUT=5000
envDone:
	Delete "$INSTDIR\shcl.exe"
	Delete "$INSTDIR\code\*.*"
	Delete "$INSTDIR\scripts\*.*"
	RMDir "$INSTDIR\code"
	RMDir "$INSTDIR\scripts"
	Delete "$INSTDIR\uninstall.exe"
	RMDir "$INSTDIR"
	DeleteRegKey HKLM "${REG_UNINST}"
SectionEnd
