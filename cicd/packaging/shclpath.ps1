## shclpath.ps1 - the setup's machine-PATH edit, packed into the installer and
## run at install and uninstall. It goes straight at the registry, never through
## NSIS variables: NSIS strings are capped at NSIS_MAX_STRLEN (1024), so a
## longer PATH came back truncated - or empty, which no length guard can tell
## from a genuinely empty value - and writing that back destroyed it. Segments
## are compared whole (a substring test let any directory containing the name
## suppress the append), the value is read unexpanded so %VAR% references
## survive, and it is written back REG_EXPAND_SZ so the type does not downgrade.
##
## Copyright (c) 2026 Jim Collier. MIT License.
param([string]$Dir, [switch]$Remove)
$key = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SYSTEM\CurrentControlSet\Control\Session Manager\Environment', $true)
$cur = [string]$key.GetValue('Path', '', [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
$parts = @($cur -split ';' | Where-Object { $_ -ne '' })
if ($Remove) {
	$new = @($parts | Where-Object { $_ -ne $Dir }) -join ';'
	if ($new -ne $cur) { $key.SetValue('Path', $new, [Microsoft.Win32.RegistryValueKind]::ExpandString) }
} elseif ($parts -notcontains $Dir) {
	$key.SetValue('Path', (@($parts + $Dir) -join ';'), [Microsoft.Win32.RegistryValueKind]::ExpandString)
}
$key.Close()
